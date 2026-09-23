// KOPNET 端点解析测试。
#include "kopnet/endpoint.hpp"

#include <cassert>
#include <cstdio>
#include <string>

using namespace kopnet;

static int failures = 0;

static void check(bool cond, const std::string& what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        ++failures;
    } else {
        std::fprintf(stderr, "ok: %s\n", what.c_str());
    }
}

int main() {
    std::string err;
    Endpoint ep;

    check(endpoint_parse("tcp://example.com:1234", &ep, &err), "tcp basic");
    check(ep.scheme == Scheme::Tcp && ep.host == "example.com" && ep.port == 1234,
          "tcp fields");
    check(ep.role == Role::Connect, "tcp default role connect");

    check(endpoint_parse("udp://224.0.0.1:5004?ttl=3", &ep, &err), "udp with param");
    check(ep.scheme == Scheme::Udp && ep.port == 5004 && ep.param("ttl") == "3",
          "udp param");

    check(endpoint_parse("rtp://media.local:5004?pt=96&ssrc=1234&clock=90000", &ep, &err),
          "rtp params");
    check(ep.param_int("pt", 0) == 96 && ep.param_int("ssrc", 0) == 1234 &&
              ep.param_int("clock", 0) == 90000,
          "rtp param values");

    check(endpoint_parse("ssh://user@10.0.0.5:2222/kopnet-relay%20--bus%20kop-0.bus", &ep,
                         &err),
          "ssh parse");
    check(ep.user == "user" && ep.host == "10.0.0.5" && ep.port == 2222, "ssh fields");
    check(ep.path == "kopnet-relay --bus kop-0.bus", "ssh remote command decoded");

    check(endpoint_parse("ssh://host", &ep, &err), "ssh default command");
    check(ep.port == 22 && ep.path == "kopnet-relay", "ssh default port+command");

    check(endpoint_parse("rdp://win.local", &ep, &err), "rdp default port");
    check(ep.port == 3389, "rdp 3389");

    check(endpoint_parse("unix:kop-0.bus", &ep, &err), "unix relative name");
    check(ep.host == "kop-0.bus", "unix name field");

    check(endpoint_parse("unix:///run/user/1000/kop-0.bus", &ep, &err), "unix abs path");
    check(ep.host == "/run/user/1000/kop-0.bus", "unix abs field");

    check(endpoint_parse("rtc://conf.example.com:8443?sid=room42", &ep, &err), "rtc parse");

    check(endpoint_parse("[::1]:9999", &ep, &err) == false, "missing scheme rejected");

    check(endpoint_parse("tcp://h:0", &ep, &err) == false, "tcp requires port");
    check(endpoint_parse("foobar://x:1", &ep, &err) == false, "unknown scheme rejected");

    // 角色由适配器层在打开时设定，解析阶段默认 Connect
    check(scheme_supports_serve(Scheme::Tcp) && scheme_supports_serve(Scheme::Unix),
          "tcp/unix support serve");
    check(!scheme_supports_serve(Scheme::Ssh) && !scheme_supports_serve(Scheme::Rdp),
          "ssh/rdp are dial-only");
    check(scheme_semantics(Scheme::Tcp) == TransportSemantics::Stream, "tcp is stream");
    check(scheme_semantics(Scheme::Udp) == TransportSemantics::Datagram, "udp is datagram");

    if (failures == 0) {
        std::fprintf(stderr, "KOPNET endpoint tests: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "KOPNET endpoint tests: %d FAILURES\n", failures);
    return 1;
}
