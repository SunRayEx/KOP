//! 媒体时钟：由音频汇聚节点驱动，向视频路径提供插值后的媒体时间。

use std::sync::Mutex;
use std::time::Instant;

struct ClockState {
    /// 最近一次上报的媒体时间（µs）
    media_us: i64,
    /// 上报发生时的单调时刻
    at: Instant,
    /// 音频路径是否已开始驱动时钟
    started: bool,
}

impl Default for ClockState {
    fn default() -> Self {
        ClockState {
            media_us: 0,
            at: Instant::now(),
            started: false,
        }
    }
}

/// 免毒化访问：任何线程上的 panic 都不会让时钟永久失效。
fn lock<T>(m: &Mutex<T>) -> std::sync::MutexGuard<'_, T> {
    match m.lock() {
        Ok(g) => g,
        Err(p) => p.into_inner(),
    }
}

pub struct MediaClock {
    st: Mutex<ClockState>,
}

impl Default for MediaClock {
    fn default() -> Self {
        MediaClock {
            st: Mutex::new(ClockState::default()),
        }
    }
}

impl MediaClock {
    /// 音频汇聚节点回调线程调用：上报当前已播放的媒体位置。
    pub fn set(&self, media_us: i64) {
        let mut g = lock(&self.st);
        g.media_us = media_us;
        g.at = Instant::now();
        g.started = true;
    }

    /// 插值当前媒体时间：上次上报值 + 上报以来的经过时间。
    pub fn now_us(&self) -> i64 {
        let g = lock(&self.st);
        if !g.started {
            return 0;
        }
        g.media_us + g.at.elapsed().as_micros() as i64
    }

    pub fn active(&self) -> bool {
        lock(&self.st).started
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::thread;
    use std::time::Duration;

    #[test]
    fn inactive_clock_reads_zero() {
        let c = MediaClock::default();
        assert!(!c.active());
        assert_eq!(c.now_us(), 0);
    }

    #[test]
    fn interpolates_between_reports() {
        let c = MediaClock::default();
        c.set(1_000_000);
        thread::sleep(Duration::from_millis(30));
        let t = c.now_us();
        // 30ms 插值 + 调度余量；保证随时间单调推进
        assert!(t >= 1_000_000 + 25_000, "clock did not advance: {t}");
        // 上报后基点前移，不再累计旧插值
        c.set(2_000_000);
        let t2 = c.now_us();
        assert!(t2 >= 2_000_000 && t2 < 2_100_000);
    }
}
