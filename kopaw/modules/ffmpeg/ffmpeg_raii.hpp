// FFmpeg 资源的 RAII 句柄。
//
// 现状（重构前）：全项目 30 处 av_frame_free、28 处 av_frame_unref、
// 24 处 av_packet_free、21 处 avfilter_graph_free、18 处
// avfilter_inout_free，全部手工配对。早返回路径漏掉一处即泄漏；异常/
// 重试路径里重复 unref 是双释放隐患。本层用唯一所有权把生命周期绑定到
// 作用域，配对由类型保证。
//
// 所有权语义（与 std::unique_ptr 一致）：
//   - 不可拷贝，可移动；移动后源对象为空
//   - 析构调用对应 free/unref，并将内部指针置空（FFmpeg 的 ** 版 free 会
//     顺带置空调用方的副本，本层不依赖该副作用）
//   - release() 让出裸指针（例如需要把对象交给 FFmpeg 持有的场景）
//
// AVFrame 有两层语义：alloc 持有对象、ref 持有缓冲区。因此额外提供
// unref()（归还缓冲区、保留对象）与 ref()/clone()（共享缓冲区）。
#pragma once

#include <utility>

#include "ffmpeg.hpp"

namespace kopaw {

namespace detail {

// 通用唯一所有权包装。Deleter 是可调用对象，接收 T* 并释放。
template <typename T, typename Deleter>
class AvPtr {
public:
    constexpr AvPtr() = default;
    explicit AvPtr(T* p) : ptr_(p) {}
    ~AvPtr() { reset(); }

    AvPtr(AvPtr&& other) noexcept : ptr_(other.release()) {}
    AvPtr& operator=(AvPtr&& other) noexcept {
        if (this != &other) reset(other.release());
        return *this;
    }
    AvPtr(const AvPtr&) = delete;
    AvPtr& operator=(const AvPtr&) = delete;

    T* get() const { return ptr_; }
    T* operator->() { return ptr_; }
    const T* operator->() const { return ptr_; }
    T& operator*() { return *ptr_; }
    const T& operator*() const { return *ptr_; }
    explicit operator bool() const { return ptr_ != nullptr; }

    T* release() {
        T* p = ptr_;
        ptr_ = nullptr;
        return p;
    }
    void reset(T* p = nullptr) {
        if (ptr_) Deleter{}(ptr_);
        ptr_ = p;
    }

private:
    T* ptr_ = nullptr;
};

struct FreeFrame {
    void operator()(AVFrame* p) const { av_frame_free(&p); }
};
struct FreePacket {
    void operator()(AVPacket* p) const { av_packet_free(&p); }
};
struct FreeCodecContext {
    void operator()(AVCodecContext* p) const { avcodec_free_context(&p); }
};
// 已 open 的格式上下文：必须用 close_input（会释放流与封装器状态）。
struct CloseFormatContext {
    void operator()(AVFormatContext* p) const { avformat_close_input(&p); }
};
// 仅 alloc 未 open 的格式上下文（如自定义 AVIO 打开失败后）。
struct FreeFormatContext {
    void operator()(AVFormatContext* p) const { avformat_free_context(p); }
};
struct FreeFilterGraph {
    void operator()(AVFilterGraph* p) const { avfilter_graph_free(&p); }
};
struct FreeFilterInOut {
    void operator()(AVFilterInOut* p) const { avfilter_inout_free(&p); }
};
struct UnrefBuffer {
    void operator()(AVBufferRef* p) const { av_buffer_unref(&p); }
};
struct FreeSws {
    void operator()(SwsContext* p) const { sws_freeContext(p); }
};
struct FreeSwr {
    void operator()(SwrContext* p) const { swr_free(&p); }
};
struct FreeAudioFifo {
    void operator()(AVAudioFifo* p) const { av_audio_fifo_free(p); }
};
struct FreeDictionary {
    void operator()(AVDictionary* p) const { av_dict_free(&p); }
};

}  // namespace detail

// AVFrame：构造即 av_frame_alloc；alloc 失败时为空，使用方须判空。
class AvFrame : public detail::AvPtr<AVFrame, detail::FreeFrame> {
public:
    AvFrame() : detail::AvPtr<AVFrame, detail::FreeFrame>(av_frame_alloc()) {}
    explicit AvFrame(AVFrame* p)
        : detail::AvPtr<AVFrame, detail::FreeFrame>(p) {}

    // 归还引用的缓冲区，保留帧对象本身（复用于循环）。
    void unref() { av_frame_unref(get()); }

    // 引用另一帧的缓冲区（共享，引用计数）。失败返回 false。
    bool ref(const AVFrame* src) {
        return av_frame_ref(get(), src) >= 0;
    }
    // 与 ref 等价，但返回新 AvFrame；失败返回空 AvFrame。
    static AvFrame clone(const AVFrame* src) {
        if (!src) return AvFrame();
        AVFrame* c = av_frame_clone(src);
        return AvFrame(c);
    }
    bool is_writable() const { return av_frame_is_writable(get()) != 0; }
    bool make_writable() { return av_frame_make_writable(get()) >= 0; }
};

class AvPacket : public detail::AvPtr<AVPacket, detail::FreePacket> {
public:
    AvPacket() : detail::AvPtr<AVPacket, detail::FreePacket>(av_packet_alloc()) {}
    explicit AvPacket(AVPacket* p)
        : detail::AvPtr<AVPacket, detail::FreePacket>(p) {}

    void unref() { av_packet_unref(get()); }
    // 分配指定大小的载荷；失败返回 false。
    bool new_packet(int size) { return av_new_packet(get(), size) >= 0; }
    static AvPacket clone(const AVPacket* src) {
        if (!src) return AvPacket();
        AVPacket* c = av_packet_clone(src);
        return AvPacket(c);
    }
};

using AvCodecContext = detail::AvPtr<AVCodecContext, detail::FreeCodecContext>;
// 默认：用 avformat_close_input。仅 alloc 未 open 时改用
// AvFormatContextOwned（见下）。
using AvFormatContext = detail::AvPtr<AVFormatContext, detail::CloseFormatContext>;
using AvFormatContextOwned =
    detail::AvPtr<AVFormatContext, detail::FreeFormatContext>;
using AvFilterGraph = detail::AvPtr<AVFilterGraph, detail::FreeFilterGraph>;
using AvFilterInOut = detail::AvPtr<AVFilterInOut, detail::FreeFilterInOut>;
using AvBufferRef = detail::AvPtr<AVBufferRef, detail::UnrefBuffer>;
using AvSws = detail::AvPtr<SwsContext, detail::FreeSws>;
using AvSwr = detail::AvPtr<SwrContext, detail::FreeSwr>;
using AvAudioFifo = detail::AvPtr<AVAudioFifo, detail::FreeAudioFifo>;
using AvDictionary = detail::AvPtr<AVDictionary, detail::FreeDictionary>;

}  // namespace kopaw
