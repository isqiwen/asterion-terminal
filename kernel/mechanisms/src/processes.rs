//! Internal process-group ownership shared by transport and supervision.
use nix::{
    errno::Errno,
    sys::signal::{Signal, killpg},
    unistd::Pid,
};
use std::{
    io,
    os::unix::process::CommandExt,
    process::{Child, Command},
};

pub(crate) fn spawn(mut command: Command) -> io::Result<Child> {
    command.process_group(0).spawn()
}

pub(crate) fn signal(child: &Child, signal: Signal) -> io::Result<()> {
    match killpg(Pid::from_raw(child.id() as i32), signal) {
        Ok(()) | Err(Errno::ESRCH) => Ok(()),
        Err(error) => Err(io::Error::from_raw_os_error(error as i32)),
    }
}
