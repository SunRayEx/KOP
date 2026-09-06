//! kopaw-core：KOPAW（Kongar Pipe Audio/Video Wire）管线图引擎。
//!
//! 职责：媒体图（节点/链路）、有界队列背压、媒体时钟、每节点一线程调度。
//! 与 C++ 节点实现之间通过 `ffi` 模块定义的 C ABI 交互（头文件由 cbindgen 生成）。

pub mod clock;
pub mod ffi;
pub mod graph;
pub mod queue;
pub mod scheduler;
