#include "esp_tcp.h"

#include <esp_log.h>
#include <esp_heap_caps.h>
#include <freertos/idf_additions.h>
#include <unistd.h>
#include <cstring>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>
#include <errno.h>

static const char *TAG = "EspTcp";

EspTcp::EspTcp() {
    event_group_ = xEventGroupCreate();
}

EspTcp::~EspTcp() {
    Disconnect();

    if (event_group_ != nullptr) {
        vEventGroupDelete(event_group_);
        event_group_ = nullptr;
    }
}

bool EspTcp::Connect(const std::string& host, int port) {
    // 确保先断开已有连接
    if (connected_) {
        Disconnect();
    }

    struct sockaddr_in server_addr;
    bzero(&server_addr, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    // host is domain
    struct hostent *server = gethostbyname(host.c_str());
    if (server == NULL) {
        last_error_ = h_errno;
        ESP_LOGE(TAG, "Failed to get host by name");
        return false;
    }
    memcpy(&server_addr.sin_addr, server->h_addr, server->h_length);
    ESP_LOGI(TAG, "Resolved %s -> %s", host.c_str(), inet_ntoa(*(struct in_addr *)server->h_addr));

    tcp_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (tcp_fd_ < 0) {
        last_error_ = errno;
        ESP_LOGE(TAG, "Failed to create socket");
        return false;
    }

    // 接收超时 1s：保证 DoDisconnect 后接收任务能及时退出，
    // 避免 fd 被后续连接复用时僵尸 recv 抢走新连接的数据
    // （需 CONFIG_LWIP_SO_RCVTIMEO=y，否则 setsockopt 无效）
    struct timeval recv_timeout;
    recv_timeout.tv_sec = 1;
    recv_timeout.tv_usec = 0;
    if (setsockopt(tcp_fd_, SOL_SOCKET, SO_RCVTIMEO, &recv_timeout, sizeof(recv_timeout)) < 0) {
        ESP_LOGW(TAG, "SO_RCVTIMEO not applied (CONFIG_LWIP_SO_RCVTIMEO enabled?), errno=%d", errno);
    }

    int ret = connect(tcp_fd_, (struct sockaddr*)&server_addr, sizeof(server_addr));
    if (ret < 0) {
        last_error_ = errno;
        ESP_LOGE(TAG, "Failed to connect to %s:%d, code=0x%x", host.c_str(), port, last_error_);
        close(tcp_fd_);
        tcp_fd_ = -1;
        return false;
    }

    connected_ = true;

    xEventGroupClearBits(event_group_, ESP_TCP_EVENT_RECEIVE_TASK_EXIT);
    BaseType_t task_ret = xTaskCreate([](void* arg) {
        EspTcp* tcp = (EspTcp*)arg;
        tcp->ReceiveTask();
        xEventGroupSetBits(tcp->event_group_, ESP_TCP_EVENT_RECEIVE_TASK_EXIT);
        vTaskDelete(NULL);
    }, "tcp_receive", 4096, this, 1, &receive_task_handle_);
    if (task_ret != pdPASS) {
        // [本地补丁] 唤醒建连时音频管道（AFE/Opus ~70KB 内部 RAM）常把内部堆压到
        // <10KB，内部栈创建必失败导致"无法连接服务"。接收任务无 IRAM/DMA 要求，
        // 内部 RAM 不足时回退 PSRAM 栈（升级组件后需重打此补丁）
        ESP_LOGW(TAG, "tcp_receive: internal stack alloc failed (free=%u), fallback to PSRAM",
                 heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        task_ret = xTaskCreateWithCaps([](void* arg) {
            EspTcp* tcp = (EspTcp*)arg;
            tcp->ReceiveTask();
            xEventGroupSetBits(tcp->event_group_, ESP_TCP_EVENT_RECEIVE_TASK_EXIT);
            vTaskDelete(NULL);
        }, "tcp_receive", 4096, this, 1, &receive_task_handle_, MALLOC_CAP_SPIRAM);
    }
    if (task_ret != pdPASS) {
        // 内存紧张时任务创建会失败：无接收任务=响应无人读取（握手超时），
        // 且 Disconnect 等 EXIT 位必超时。必须检查并中止连接
        ESP_LOGE(TAG, "Failed to create receive task, free internal heap=%u", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        connected_ = false;
        shutdown(tcp_fd_, SHUT_RDWR);
        close(tcp_fd_);
        tcp_fd_ = -1;
        return false;
    }
    return true;
}

void EspTcp::Disconnect() {
    // 如果已经断开，直接返回
    if (!connected_) {
        return;
    }

    // 主动断开，需要等待接收任务退出
    DoDisconnect(true);
}

void EspTcp::DoDisconnect(bool wait_for_task) {
    connected_ = false;

    if (tcp_fd_ != -1) {
        // 先 shutdown 唤醒阻塞在 recv 上的接收任务再 close；
        // 否则接收任务可能永久阻塞，fd 号被后续连接复用后会读到新连接的数据
        shutdown(tcp_fd_, SHUT_RDWR);
        close(tcp_fd_);
        tcp_fd_ = -1;

        // 只有主动断开时才需要等待接收任务退出
        // 被动断开时，当前就是接收任务，不需要等待
        if (wait_for_task) {
            auto bits = xEventGroupWaitBits(event_group_, ESP_TCP_EVENT_RECEIVE_TASK_EXIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(10000));
            if (!(bits & ESP_TCP_EVENT_RECEIVE_TASK_EXIT)) {
                ESP_LOGE(TAG, "Failed to wait for receive task exit");
            }
        }
    }

    // 断开连接时触发断开回调
    if (disconnect_callback_) {
        disconnect_callback_();
    }
}

int EspTcp::Send(const std::string& data) {
    if (!connected_) {
        ESP_LOGE(TAG, "Not connected");
        return -1;
    }

    size_t total_sent = 0;
    size_t data_size = data.size();
    const char* data_ptr = data.data();

    while (total_sent < data_size) {
        int ret = send(tcp_fd_, data_ptr + total_sent, data_size - total_sent, 0);

        if (ret <= 0) {
            ESP_LOGE(TAG, "Send failed: ret=%d, errno=%d", ret, errno);
            return ret;
        }

        total_sent += ret;
    }

    return total_sent;
}

void EspTcp::ReceiveTask() {
    std::string data;
    while (connected_) {
        data.resize(1500);
        int ret = recv(tcp_fd_, data.data(), data.size(), 0);
        if (ret < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // 接收超时：循环检查 connected_，Disconnect 后据此退出
            continue;
        }
        if (ret <= 0) {
            if (ret < 0) {
                ESP_LOGE(TAG, "TCP receive failed: %d", ret);
            }
            // 被动断开，不需要等待接收任务退出（当前就是接收任务）
            DoDisconnect(false);
            break;
        }

        if (stream_callback_) {
            data.resize(ret);
            stream_callback_(data);
        }
    }
}

int EspTcp::GetLastError() {
    return last_error_;
}
