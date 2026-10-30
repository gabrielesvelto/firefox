/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

use anyhow::Result;
use crash_helper_common::ProcessHandle;
use nix::{
    libc::_exit,
    unistd::{fork, getpid, setsid, write, ForkResult},
};
use std::{
    ffi::{OsStr, OsString},
    io::stdout,
    os::fd::AsFd,
    path::PathBuf,
};

pub(crate) const PROXY_RENDEZ_VOUS: bool = false;

// Daemonize the current process by forking it and then immediately returning
// in the parent. This should have been done via a double fork() in the
// crash_helper_client crate, however the first fork() call causes issues to
// Thunderbird on macOS 10.15 (see bug 1977514). This is a known problem with
// macOS 10.15 implemenetation, not a flaw in our logic, and the only way to
// work around it is to use posix_spawn() instead, which forces use to move
// the step to reparent the crash helper to PID 1 here.
//
// Note that if this fails for some reason, the crash helper will still launch,
// but not as a daemon. Not ideal but still better to have a fallback.
//
// # Safety
//
// This calls fork() which can only be done safely in a non-multi-threaded
// environment. This is something that the caller must guarantee. If we have
// spawned any other threads before calling this function then things might
// break in unexpected ways.
pub(crate) unsafe fn daemonize() {
    // Create a new process group and a new session, this guarantees
    // that the crash helper process will be disconnected from the
    // signals of Firefox main process' controlling terminal. Killing
    // Firefox via the terminal shouldn't kill the crash helper which
    // has its own lifecycle management.
    //
    // We don't check for errors as there's nothing we can do to
    // handle one in this context.
    let _ = setsid();

    let pid = if let Ok(res) = fork() {
        match res {
            ForkResult::Child => {
                return;
            }
            ForkResult::Parent { child } => child,
        }
    } else {
        getpid()
    };

    // We're done, write the daemonized process pid to standard output if
    // forking succeeded, or write the current process pid if it failed.
    let raw_pid = pid.as_raw();
    let raw_pid_bytes: [u8; 4] = raw_pid.to_ne_bytes();
    let rv = write(stdout().as_fd(), &raw_pid_bytes);

    _exit(if rv.is_ok_and(|rv| rv == 4) { 0 } else { 1 });
}

pub(crate) fn get_client_handle(_handle: &OsStr) -> Result<Option<ProcessHandle>> {
    Ok(None)
}

/// Turns the value of MOZ_APP_DATA into a path the way nsLocalFile does: an
/// absolute path is used as is, `~` and a path starting with `~/` are relative
/// to the home directory, and a path starting with `~user/` is relative to that
/// user's home directory. Anything else is rejected.
pub(crate) fn path_from_env_value(value: OsString) -> Option<PathBuf> {
    use std::{ffi::OsStr, os::unix::ffi::OsStrExt, path::Path};

    let bytes = value.as_bytes();
    match bytes {
        [b'/', ..] => Some(PathBuf::from(value)),
        [b'~'] | [b'~', b'/'] => dirs::home_dir(),
        [b'~', suffix @ ..] if suffix.starts_with(b"/") => {
            // Append the rest of the value as is, like nsLocalFile does, so
            // that `~//dir` stays inside the home directory instead of
            // becoming `/dir`.
            let mut path = dirs::home_dir()?.into_os_string();
            path.push(OsStr::from_bytes(suffix));
            Some(PathBuf::from(path))
        }
        [b'~', rest @ ..] => {
            let users_dir = if cfg!(target_os = "macos") {
                "/Users"
            } else {
                "/home"
            };
            Some(Path::new(users_dir).join(OsStr::from_bytes(rest)))
        }
        _ => None,
    }
}
