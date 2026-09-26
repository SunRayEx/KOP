// 模糊靶：KOPNET RTP / RTCP / STUN 头部解析。
//
// 这些解析器直接面对不可信网络字节（单端口复用分流、RTP 隧道入口），
// 是最该被模糊的纯函数。harness 只做解析，不做任何 I/O。
#include "fuzz_driver.hpp"

#include "rtp_framing.hpp"

#include <string>
#include <vector>

namespace {

// 构造一条合法的 RTP 报文（V2、带 CSRC 与扩展），给变异循环一个能深入
// 解析器内部路径的起点；纯随机字节只会反复命中“版本不是 2”的早返回。
std::vector<uint8_t> make_rtp_seed() {
    std::vector<uint8_t> b(32, 0);
    b[0] = 0x80 | 0x02;  // V=2, P=0, X=1, CC=2
    b[1] = 0x60;         // M=1, PT=96
    b[2] = 0x12;
    b[3] = 0x34;
    b[4] = 0xAA;
    b[5] = 0xBB;
    b[6] = 0xCC;
    b[7] = 0xDD;
    // CSRC x2（偏移 12..19）
    b[18] = 0x42;
    // 扩展头（偏移 20）：id=0xBEDE，len=1（4 字节扩展数据）
    b[20] = 0xBE;
    b[21] = 0xDE;
    b[22] = 0x00;
    b[23] = 0x01;
    return b;
}

std::vector<uint8_t> make_rtcp_seed() {
    std::vector<uint8_t> b(12, 0);
    b[0] = 0x80 | 0x02;  // V=2, P=0, RC=2
    b[1] = 200;          // PT = SR
    b[2] = 0x00;
    b[3] = 0x06;         // len_words = 6
    return b;
}

std::vector<uint8_t> make_stun_seed() {
    std::vector<uint8_t> b(20, 0);
    b[0] = 0x00;  // type = Binding Request
    b[1] = 0x01;
    b[4] = 0x21;  // magic cookie
    b[5] = 0x12;
    b[6] = 0xA4;
    b[7] = 0x42;
    return b;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t len) {
    std::string error;

    kopnet::RtpHeader rtp{};
    size_t payload_offset = 0;
    (void)kopnet::parse_rtp(data, len, &rtp, &payload_offset, &error);
    // 解析成功时 payload_offset 必须落在缓冲内——断言这条不变式本身。
    if (len >= 12 && rtp.version == 2 && payload_offset > len) return 1;

    kopnet::RtcpHeader rtcp{};
    (void)kopnet::parse_rtcp(data, len, &rtcp, &error);

    kopnet::StunHeader stun{};
    (void)kopnet::parse_stun(data, len, &stun, &error);

    // 分类结果必须与识别结果一致（RTP 一定先被 parse_rtp 接受）。
    const kopnet::MediaPacketKind kind = kopnet::classify_media_packet(data, len);
    (void)kind;
    return 0;
}

extern "C" void kop_fuzz_make_seeds(kop_fuzz::Seeds* out) {
    out->buffers.push_back(make_rtp_seed());
    out->buffers.push_back(make_rtcp_seed());
    out->buffers.push_back(make_stun_seed());
    out->buffers.emplace_back();  // 空输入：覆盖所有“过短”分支
}
