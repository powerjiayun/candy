// SPDX-License-Identifier: MIT
#include "core/client.h"
#include "core/common.h"
#include "core/message.h"
#include "utility/address.h"
#include "utility/time.h"
#include "utility/uri.h"
#include <bit>
#include <functional>
#include <spdlog/fmt/bin_to_hex.h>
#include <spdlog/spdlog.h>

namespace Candy {

int Client::setName(const std::string &name) {
    this->tunName = name;
    return 0;
}

int Client::setPassword(const std::string &password) {
    this->password = password;
    return 0;
}

int Client::setWebSocketServer(const std::string &uri) {
    Uri parser(uri);
    if (!parser.isValid()) {
        spdlog::critical("Client websocket server parser failed");
        return -1;
    }
    if (parser.scheme() != "ws" && parser.scheme() != "wss") {
        spdlog::critical("Invalid websocket scheme {}", parser.scheme());
        return -1;
    }
    this->wsUri = uri;
    return 0;
}

int Client::setLocalAddress(const std::string &cidr) {
    this->localAddress = cidr;
    return 0;
}

int Client::setDynamicAddress(const std::string &cidr) {
    this->dynamicAddress = cidr;
    return 0;
}

std::string Client::getAddress() {
    return this->localAddress;
}

int Client::run() {
    this->running = true;
    if (startWsThread()) {
        spdlog::critical("Start websocket client thread failed");
        return -1;
    }
    return 0;
}

int Client::shutdown() {
    if (!this->running) {
        return 0;
    }

    this->running = false;

    if (this->wsThread.joinable()) {
        this->wsThread.join();
    }
    if (this->tunThread.joinable()) {
        this->tunThread.join();
    }

    this->tun.down();
    this->ws.disconnect();
    return 0;
}

int Client::startWsThread() {
    if (this->ws.connect(this->wsUri)) {
        spdlog::critical("Websocket client connect failed");
        return -1;
    }
    if (this->ws.setTimeout(1)) {
        spdlog::critical("Websocket clinet set read write timeout failed");
        return -1;
    }

    // 只需要开 wsThread, 执行过程中会设置 tun 并开 tunThread.
    this->wsThread = std::move(std::thread(&Client::handleWebSocketMessage, this));
    return 0;
}

int Client::startTunThread() {
    if (this->tun.setName(this->tunName.empty() ? "candy" : "candy-" + this->tunName)) {
        return -1;
    }
    if (this->tun.setAddress(this->localAddress)) {
        return -1;
    }
    if (this->tun.setMTU(1400)) {
        return -1;
    }
    if (this->tun.setTimeout(1)) {
        return -1;
    }
    if (this->tun.up()) {
        return -1;
    }

    this->tunThread = std::move(std::thread(&Client::handleTunMessage, this));

    sendAuthMessage();
    return 0;
}

void Client::handleWebSocketMessage() {
    int error;
    WebSocketMessage message;

    while (this->running) {
        error = this->ws.read(message);

        if (error == 0) {
            continue;
        }
        if (error < 0) {
            spdlog::critical("WebSocket client read failed: error={0}", error);
            break;
        }
        if (message.type == WebSocketMessageType::Message) {
            // TYPE_FORWARD, 拆包后转发给 TUN 设备
            if (message.buffer.front() == MessageType::TYPE_FORWARD) {
                handleForwardMessage(message);
                continue;
            }
            // 收到动态地址响应包,启动 TUN 设备并发送 Auth 包
            if (message.buffer.front() == MessageType::TYPE_DYNAMIC_ADDRESS) {
                handleDynamicAddressMessage(message);
                continue;
            }
            spdlog::warn("Unknown message type. type={0}", message.buffer.front());
            continue;
        }

        if (message.type == WebSocketMessageType::Open) {
            if (!this->localAddress.empty()) {
                if (startTunThread()) {
                    spdlog::critical("Start tun thread failed");
                    break;
                }
                continue;
            }

            Address address;
            if (this->dynamicAddress.empty() || address.cidrUpdate(this->dynamicAddress)) {
                this->dynamicAddress = "0.0.0.0/0";
                spdlog::warn("Invalid dynamic address, set dynamic address to {}", this->dynamicAddress);
            }
            sendDynamicAddressMessage();
            continue;
        }
        // 连接断开,可能是地址冲突,触发正常退出进程的流程
        if (message.type == WebSocketMessageType::Close) {
            spdlog::info("WebSocket communication disconnected");
            break;
        }
        // 通信出现错误,触发正常退出进程的流程
        if (message.type == WebSocketMessageType::Error) {
            spdlog::critical("WebSocket communication exception");
            break;
        }
    }
    Candy::shutdown();
    return;
}

void Client::handleTunMessage() {
    int error;
    WebSocketMessage message;
    std::string buffer;
    IPv4Header *header;

    while (this->running) {
        error = this->tun.read(buffer);
        if (error == 0) {
            continue;
        }
        if (error < 0) {
            spdlog::critical("Tun read failed. error={0}", error);
            break;
        }
        if (buffer.length() < sizeof(IPv4Header)) {
            continue;
        }

        // 仅处理 IPv4
        header = (IPv4Header *)buffer.data();
        if ((header->version_ihl >> 4) != 4) {
            continue;
        }
        // 发包地址必须与登录地址相同
        if (Address::netToHost(header->saddr) != this->tun.getIP()) {
            continue;
        }

        // 目前客户端只与服务端通信,所以可以不加判断的直接把数据发给服务端.
        // 未来支持端到端通信后,发送数据前先判断到目的地址能否直连,并通过直连的连接发送
        message.buffer.clear();
        message.buffer.push_back(MessageType::TYPE_FORWARD);
        message.buffer.append(buffer);
        ws.write(message);
    }
    Candy::shutdown();
    return;
}

void Client::sendDynamicAddressMessage() {
    Address address;
    address.cidrUpdate(this->dynamicAddress);

    DynamicAddressHeader header(address.getCidr());
    header.updateHash(this->password);

    WebSocketMessage message;
    message.buffer.assign((char *)(&header), sizeof(DynamicAddressHeader));
    this->ws.write(message);
    return;
}

void Client::sendAuthMessage() {
    Address address;
    address.cidrUpdate(this->localAddress);

    AuthHeader header(address.getIp());
    header.updateHash(this->password);

    WebSocketMessage message;
    message.buffer.assign((char *)(&header), sizeof(AuthHeader));
    this->ws.write(message);
    return;
}

void Client::handleDynamicAddressMessage(WebSocketMessage &message) {
    if (message.buffer.size() != sizeof(DynamicAddressHeader)) {
        spdlog::warn("Invalid dynamic address package: len={}", message.buffer.length());
        spdlog::debug("Dynamic address buffer: {:n}", spdlog::to_hex(message.buffer));
        return;
    }

    DynamicAddressHeader *header = (DynamicAddressHeader *)message.buffer.c_str();

    Address address;
    if (address.cidrUpdate(header->cidr)) {
        spdlog::warn("Invalid dynamic address ip: cidr={}", header->cidr);
        return;
    }

    this->localAddress = address.getCidr();
    startTunThread();
}

void Client::handleForwardMessage(WebSocketMessage &message) {
    if (message.buffer.size() < sizeof(ForwardHeader)) {
        spdlog::warn("Invalid forward package: {:n}", spdlog::to_hex(message.buffer));
    }

    const char *src = message.buffer.c_str() + sizeof(ForwardHeader::type);
    const size_t len = message.buffer.length() - sizeof(ForwardHeader::type);
    this->tun.write(std::string(src, len));
}

}; // namespace Candy