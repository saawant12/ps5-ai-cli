//! Verify the pipe transport used by Codex shell tools, including backpressure.
use std::collections::HashMap;
use std::io;
use std::path::Path;
use std::time::Duration;

use codex_utils_pty::spawn_pipe_process;

pub async fn run(directory: &Path) -> io::Result<()> {
    let input = b"before\n".repeat(16_384);
    let expected = b"after\n".repeat(16_384);
    std::fs::write(directory.join("task-input"), &input)?;
    std::fs::write(directory.join("task-expected"), &expected)?;
    let env = HashMap::from([
        ("PATH".to_owned(), directory.to_string_lossy().into_owned()),
        (
            "SHELL".to_owned(),
            directory.join("sh").to_string_lossy().into_owned(),
        ),
    ]);
    let script = "set -e; IFS= read -r value; [ \"$value\" = 'input verified' ]; \
                  cat task-input; sed 's/before/after/' task-input > task-edited; \
                  cmp task-edited task-expected; printf check-ok; printf pipe-error >&2";
    let spawned = spawn_pipe_process(
        "sh",
        &["-c".to_owned(), script.to_owned()],
        directory,
        &env,
        &None,
        &[],
    )
    .await
    .map_err(io::Error::other)?;
    let session = spawned.session;
    session
        .writer_sender()
        .send(b"input verified\n".to_vec())
        .await
        .map_err(io::Error::other)?;
    session.close_stdin();
    let capture = |mut receiver: tokio::sync::mpsc::Receiver<Vec<u8>>| {
        tokio::spawn(async move {
            let mut bytes = Vec::new();
            while let Some(chunk) = receiver.recv().await {
                if bytes.len() + chunk.len() > 256 * 1024 {
                    return Err(io::Error::other("probe output exceeded its bound"));
                }
                bytes.extend(chunk);
            }
            Ok(bytes)
        })
    };
    let stdout = capture(spawned.stdout_rx);
    let stderr = capture(spawned.stderr_rx);
    let outcome = tokio::time::timeout(Duration::from_secs(30), async {
        Ok::<_, io::Error>((
            stdout.await.map_err(io::Error::other)??,
            stderr.await.map_err(io::Error::other)??,
            spawned.exit_rx.await.map_err(io::Error::other)?,
        ))
    })
    .await
    .map_err(io::Error::other)?;
    let (stdout, stderr, status) = outcome?;
    drop(session);
    let edited = std::fs::read(directory.join("task-edited"))?;
    for name in ["task-input", "task-expected", "task-edited"] {
        std::fs::remove_file(directory.join(name))?;
    }
    let mut expected_stdout = input;
    expected_stdout.extend_from_slice(b"check-ok");
    if status != 0 || stdout != expected_stdout || stderr != b"pipe-error" || edited != expected {
        return Err(io::Error::other(format!(
            "pipe file-edit/backpressure check failed: status={status}, stdout={}/{}, stdout_matches={}, stderr={:?}, edited={}/{}, edited_matches={}",
            stdout.len(),
            expected_stdout.len(),
            stdout == expected_stdout,
            String::from_utf8_lossy(&stderr[..stderr.len().min(8192)]),
            edited.len(),
            expected.len(),
            edited == expected,
        )));
    }
    println!("Codex pipe transport: stdin + large stdout + stderr + file edit + check: PASS");
    Ok(())
}
