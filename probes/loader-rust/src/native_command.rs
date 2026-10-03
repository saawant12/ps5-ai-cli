//! Exercise the same command implementation linked into the native Codex CLI.
use std::io;
use std::os::unix::ffi::OsStrExt;
use std::os::unix::fs::PermissionsExt;
use std::path::Path;
use std::time::Duration;

use codex_utils_pty::{Command, DescriptorPolicy, ProcessMode};
use tokio::io::{AsyncReadExt, AsyncWriteExt};

fn command(program: &Path, directory: &Path) -> Command {
    let mut request = Command::new(program);
    request
        .current_dir(directory)
        .descriptor_policy(DescriptorPolicy::Explicit)
        .process_mode(ProcessMode::NewSession)
        .env("PATH", directory)
        .env("SHELL", directory.join("sh"));
    request
}

pub async fn run(directory: &Path) -> io::Result<()> {
    let mut request = command(Path::new("sh"), directory);
    request.args([
        "-c",
        "IFS= read -r value; printf '%s\\n' \"$value\"; printf error >&2; exit 37",
    ]);
    let mut child = request.spawn()?;
    child
        .stdin
        .as_mut()
        .unwrap()
        .write_all(b"native command\n")
        .await?;
    let output = child.wait_with_output().await?;
    if output.status.code() != Some(37)
        || output.stdout != b"native command\n"
        || output.stderr != b"error"
    {
        return Err(io::Error::other(format!(
            "native command output mismatch: {output:?}"
        )));
    }
    println!("Codex Command: PATH + cwd + async stdin/stdout/stderr + exit status: PASS");

    let missing = command(&directory.join("missing-program"), directory).spawn();
    if missing.err().and_then(|error| error.raw_os_error()) != Some(libc::ENOENT) {
        return Err(io::Error::other("missing executable did not return ENOENT"));
    }
    let bad = directory.join("invalid-elf");
    std::fs::write(&bad, b"not a PS5 executable")?;
    std::fs::set_permissions(&bad, std::fs::Permissions::from_mode(0o700))?;
    let rejected = command(&bad, directory).spawn().err();
    std::fs::remove_file(&bad)?;
    if rejected.and_then(|error| error.raw_os_error()) != Some(libc::ENOEXEC) {
        return Err(io::Error::other(
            "malformed executable did not return ENOEXEC",
        ));
    }
    let nul = std::ffi::OsStr::from_bytes(b"bad\0argument");
    let mut invalid = command(&directory.join("helper"), directory);
    invalid.arg(nul);
    if invalid.spawn().err().map(|error| error.kind()) != Some(io::ErrorKind::InvalidInput) {
        return Err(io::Error::other("NUL argument was not rejected"));
    }
    println!("Codex Command: missing file + malformed ELF + invalid argument: PASS");

    let mut request = command(&directory.join("helper"), directory);
    request.arg("--wait");
    let mut child = request.spawn()?;
    let mut ready = [0; 6];
    child
        .stdout
        .as_mut()
        .unwrap()
        .read_exact(&mut ready)
        .await?;
    if ready != *b"ready\n"
        || tokio::time::timeout(Duration::from_millis(20), child.wait())
            .await
            .is_ok()
    {
        return Err(io::Error::other(
            "cancellable wait did not retain its live child",
        ));
    }
    child.kill().await?;
    if child.id().is_some() || child.wait().await?.success() {
        return Err(io::Error::other(
            "killed child status or ownership incorrect",
        ));
    }
    println!("Codex Command: cancel wait + kill + cached reaped status: PASS");

    let mut request = command(&directory.join("helper"), directory);
    request.arg("--wait");
    let mut child = request.spawn()?;
    child
        .stdout
        .as_mut()
        .unwrap()
        .read_exact(&mut ready)
        .await?;
    let pid = child.id().unwrap() as i32;
    drop(child);
    for _ in 0..200 {
        // Read-only existence check; the native reaper exclusively owns waitpid.
        if unsafe { libc::kill(pid, 0) } < 0
            && io::Error::last_os_error().raw_os_error() == Some(libc::ESRCH)
        {
            println!("Codex Command: dropped child killed and reaped: PASS");
            return Ok(());
        }
        tokio::time::sleep(Duration::from_millis(10)).await;
    }
    Err(io::Error::other("dropped command was not reaped"))
}
