//! Run native child-launch checks while Tokio workers and timers remain active.
use std::ffi::{CStr, CString, c_char};
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::time::Duration;

unsafe extern "C" {
    fn ps5_probe_spawn_loader(directory: *const c_char, mode: i32) -> i32;
}

/// # Safety
/// `directory` must point to a valid NUL-terminated string for this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ps5_rust_loader_probe(directory: *const c_char) -> i32 {
    if directory.is_null() {
        return 1;
    }
    let directory = unsafe { CStr::from_ptr(directory) }.to_owned();
    let runtime = match tokio::runtime::Builder::new_multi_thread()
        .worker_threads(2)
        .thread_stack_size(16 * 1024 * 1024)
        .enable_all()
        .build()
    {
        Ok(runtime) => runtime,
        Err(error) => {
            println!("Rust loader runtime failed: {error}");
            return 1;
        }
    };
    runtime.block_on(async move {
        let parent_env = std::env::var_os("PS5_PROBE_VALUE");
        let parent_cwd = std::env::current_dir().ok();
        let ticks = Arc::new(AtomicUsize::new(0));
        let stop = Arc::new(AtomicBool::new(false));
        let task_ticks = Arc::clone(&ticks);
        let task_stop = Arc::clone(&stop);
        let heartbeat = tokio::spawn(async move {
            while !task_stop.load(Ordering::Acquire) {
                let work = format!("allocation while child starts: {:?}", vec![42_u64; 256]);
                std::hint::black_box(work);
                task_ticks.fetch_add(1, Ordering::Release);
                tokio::time::sleep(Duration::from_millis(2)).await;
            }
        });
        while ticks.load(Ordering::Acquire) == 0 {
            tokio::task::yield_now().await;
        }
        let started = ticks.load(Ordering::Acquire);
        let mut failed = false;
        for mode in [0, 1, 2] {
            let child_directory: CString = directory.clone();
            let result = tokio::task::spawn_blocking(move || unsafe {
                ps5_probe_spawn_loader(child_directory.as_ptr(), mode)
            })
            .await;
            println!("Rust native loader case {mode}: {result:?}");
            if !matches!(result, Ok(0)) {
                failed = true;
                break;
            }
        }
        let active_ticks = ticks.load(Ordering::Acquire) - started;
        stop.store(true, Ordering::Release);
        if heartbeat.await.is_err() {
            failed = true;
        }
        println!("Tokio allocation/timer ticks during native launch: {active_ticks}");
        failed |= active_ticks == 0;
        let context_unchanged = parent_env == std::env::var_os("PS5_PROBE_VALUE")
            && parent_cwd.is_some()
            && parent_cwd == std::env::current_dir().ok();
        println!("Parent cwd and environment unchanged: {context_unchanged}");
        failed |= !context_unchanged;
        println!(
            "Rust + Tokio + native child loader: {}",
            if failed { "FAIL" } else { "PASS" }
        );
        i32::from(failed)
    })
}
