//! Exercises actual Rust std on the console before building Codex against it.
use std::io::{Read, Write};
use std::sync::{Arc, Mutex};

#[unsafe(no_mangle)]
pub extern "C" fn ps5_rust_probe() -> i32 {
    println!("Rust std probe: entered (this is not Codex)");
    let text = format!("allocation: {}", [1, 2, 3].iter().sum::<u32>());
    println!("{text}");
    println!("current directory: {:?}", std::env::current_dir());
    println!("time: {:?}", std::time::SystemTime::now());
    let path = "/data/ps5-ai-cli/rust-probe.tmp";
    let file_test = (|| -> std::io::Result<()> {
        // Never truncate a preexisting file, including another probe's output.
        let mut file = std::fs::OpenOptions::new()
            .create_new(true)
            .write(true)
            .open(path)?;
        file.write_all(b"PS5 Rust std probe\n")?;
        drop(file);
        let mut contents = String::new();
        std::fs::File::open(path)?.read_to_string(&mut contents)?;
        std::fs::remove_file(path)?;
        if contents != "PS5 Rust std probe\n" {
            return Err(std::io::Error::other("file roundtrip mismatch"));
        }
        Ok(())
    })();
    println!("file roundtrip: {file_test:?}");
    let value = Arc::new(Mutex::new(0));
    let child_value = Arc::clone(&value);
    let child = std::thread::Builder::new()
        .name("ps5-rust-probe".into())
        .spawn(move || {
            *child_value.lock().unwrap() = 42;
        });
    let thread_ok = match child {
        Ok(handle) => handle.join().is_ok() && *value.lock().unwrap() == 42,
        Err(error) => {
            println!("thread creation failed: {error}");
            false
        }
    };
    println!("thread + mutex: {thread_ok}");
    let sleep_ok = [0, 1, 25, 25].into_iter().all(|millis| {
        let duration = std::time::Duration::from_millis(millis);
        let started = std::time::Instant::now();
        std::thread::sleep(duration);
        let elapsed = started.elapsed();
        println!(
            "Rust thread sleep {millis}ms: elapsed={}us",
            elapsed.as_micros()
        );
        elapsed >= duration && elapsed < std::time::Duration::from_secs(5)
    });
    println!(
        "directory iteration: {:?}",
        std::fs::read_dir("/data/ps5-ai-cli").map(|entries| entries.count())
    );
    println!("current executable: {:?}", std::env::current_exe());
    println!("starting Tokio runtime");
    let async_test = (|| -> std::io::Result<()> {
        let runtime = tokio::runtime::Builder::new_multi_thread()
            .worker_threads(2)
            .thread_stack_size(16 * 1024 * 1024)
            .enable_all()
            .build()?;
        runtime.block_on(async {
            use tokio::io::{AsyncReadExt, AsyncWriteExt};
            tokio::time::timeout(std::time::Duration::from_secs(5), async {
                let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await?;
                let address = listener.local_addr()?;
                let client = tokio::spawn(async move {
                    let mut socket = tokio::net::TcpStream::connect(address).await?;
                    socket.write_all(b"ps5-async").await?;
                    Ok::<_, std::io::Error>(())
                });
                let (mut socket, _) = listener.accept().await?;
                let mut data = [0; 9];
                socket.read_exact(&mut data).await?;
                client.await.map_err(std::io::Error::other)??;
                if &data != b"ps5-async" {
                    return Err(std::io::Error::other("async TCP roundtrip mismatch"));
                }
                tokio::time::sleep(std::time::Duration::from_millis(10)).await;
                Ok::<_, std::io::Error>(())
            })
            .await
            .map_err(std::io::Error::other)?
        })
    })();
    println!("Tokio timers + async TCP: {async_test:?}");
    i32::from(file_test.is_err() || !thread_ok || !sleep_ok || async_test.is_err())
}
