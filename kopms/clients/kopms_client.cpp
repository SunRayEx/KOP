// KOPMS-C diagnostic client: negotiate the native BUS2LAYER protocol.
#include <cstdio>
#include <cstdlib>
#include <string>

#include "kopms_client.h"

namespace {

void usage(const char* program) {
    std::fprintf(stderr, "usage: %s [BUS_SOCKET]\n", program ? program : "kopms-client");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 2 || (argc == 2 && std::string(argv[1]) == "--help")) {
        usage(argc > 0 ? argv[0] : nullptr);
        return argc == 2 ? 0 : 2;
    }
    const std::string socket_name = argc == 2 ? argv[1] : "kopms-bus-0";
    kopms::KopmsClient client;
    std::string error;
    if (!client.connect(socket_name, &error) ||
        !client.hello(KOPMS_PROTOCOL_CAP_DMABUF | KOPMS_PROTOCOL_CAP_FRAME_RELEASE |
                          KOPMS_PROTOCOL_CAP_WAYLAND_BRIDGE |
                          KOPMS_PROTOCOL_CAP_CONTROL_STATE,
                      &error)) {
        std::fprintf(stderr, "KOPMS-C: %s\n", error.c_str());
        return 1;
    }
    std::printf("KOPMS-C handshake complete caps=0x%llx minor=%u socket=%s\n",
                static_cast<unsigned long long>(client.capabilities()),
                client.negotiated_minor(), socket_name.c_str());
    client.disconnect();
    return 0;
}
