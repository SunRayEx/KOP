// KOPNET 隧道线协议助手：显式小端读写。
//
// 线格式一律小端、显式长度前缀；host 结构体的 C++ padding 绝不进入线格式。
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace kopnet {

// 小端写入器
class WireWriter {
public:
    void u8(uint8_t v) { buf_.push_back(v); }
    void u16(uint16_t v) {
        buf_.push_back(static_cast<uint8_t>(v & 0xff));
        buf_.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
    }
    void u32(uint32_t v) {
        for (int i = 0; i < 4; ++i)
            buf_.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xff));
    }
    void u64(uint64_t v) {
        for (int i = 0; i < 8; ++i)
            buf_.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xff));
    }
    void bytes(const uint8_t* p, size_t n) { buf_.insert(buf_.end(), p, p + n); }
    void bytes(const std::string& s) { buf_.insert(buf_.end(), s.begin(), s.end()); }
    void bytes(const std::vector<uint8_t>& v) { buf_.insert(buf_.end(), v.begin(), v.end()); }
    void pad_to(size_t len) {
        while (buf_.size() < len) buf_.push_back(0);
    }
    const std::vector<uint8_t>& data() const { return buf_; }
    std::vector<uint8_t> take() { return std::move(buf_); }

private:
    std::vector<uint8_t> buf_;
};

// 小端读取器（越界返回 false）
class WireReader {
public:
    explicit WireReader(const std::vector<uint8_t>& data) : data_(data) {}
    explicit WireReader(const uint8_t* p, size_t n)
        : data_(p, p + n) {}

    bool u8(uint8_t* out) {
        if (pos_ + 1 > data_.size()) return false;
        *out = data_[pos_++];
        return true;
    }
    bool u16(uint16_t* out) {
        if (pos_ + 2 > data_.size()) return false;
        *out = static_cast<uint16_t>(data_[pos_]) |
               (static_cast<uint16_t>(data_[pos_ + 1]) << 8);
        pos_ += 2;
        return true;
    }
    bool u32(uint32_t* out) {
        if (pos_ + 4 > data_.size()) return false;
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(data_[pos_ + i]) << (8 * i);
        *out = v;
        pos_ += 4;
        return true;
    }
    bool u64(uint64_t* out) {
        if (pos_ + 8 > data_.size()) return false;
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(data_[pos_ + i]) << (8 * i);
        *out = v;
        pos_ += 8;
        return true;
    }
    bool bytes(std::vector<uint8_t>* out, size_t n) {
        if (pos_ + n > data_.size()) return false;
        out->assign(data_.begin() + pos_, data_.begin() + pos_ + n);
        pos_ += n;
        return true;
    }
    bool skip(size_t n) {
        if (pos_ + n > data_.size()) return false;
        pos_ += n;
        return true;
    }
    size_t remaining() const { return data_.size() - pos_; }

private:
    std::vector<uint8_t> data_;
    size_t pos_ = 0;
};

}  // namespace kopnet
