/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#[cfg(any(target_os = "linux", target_os = "android"))]
extern crate rust_minidump_writer_linux;

mod breakpad_crash_generator;
mod crash_generation;
mod ipc_server;
mod logging;
mod memory_info;
mod phc;
mod platform;

use crash_helper_common::{
    ApplicationInfo, BreakpadChar, BreakpadData, BreakpadRawData, BreakpadString, IPCConnector,
    IPCListener, Pid,
};
use std::{
    env,
    ffi::OsString,
    path::{Path, PathBuf},
};

use crash_generation::{finalize_breakpad_minidump, initialize_static_annotations};
use ipc_server::{ClientEnvironment, IPCServer, IPCServerState};
#[cfg(not(target_os = "android"))]
use platform::CRASH_REPORTER_PATH;

// Indicates how the client is operating. When in background mode the crash
// helper will not prompt the user if the client crashes.
#[derive(Clone, Copy)]
enum ClientMode {
    Foreground,
    #[allow(unused)] // Unused on Android
    Background,
}

/// Runs the crash generator process logic, this includes the IPC used by
/// processes to signal that they crashed, the IPC used to retrieve crash
/// reports from the crash helper process and the logic used to generate the
/// actual minidumps. This function will return when the main process has
/// disconnected from the crash helper.
///
/// # Safety
///
/// `minidump_data`, `listener` and `pipe` must point to valid,
/// nul-terminated C strings. `breakpad_data` must be a valid file descriptor
/// (Linux) or point to a nul-terminated C string using either byte (macOS)
/// or wide characters (Windows).
#[cfg(not(target_os = "android"))]
#[no_mangle]
pub unsafe extern "C" fn crash_generator_logic_desktop(
    client_pid: Pid,
    client_handle: *const BreakpadChar,
    breakpad_data: BreakpadRawData,
    minidump_path: *const BreakpadChar,
    build_id: *const BreakpadChar,
    background_mode: bool,
    listener: *const BreakpadChar,
    pipe: *const BreakpadChar,
) -> i32 {
    // HACK: This constant is declared in the `mach2` crate but using a `c_uint`
    // type which makes it incompatible with the return value of
    // `bootstrap_look_up()` and thus prevents the `match` expression below from
    // compiling correctly. We re-declare it here as a `c_int` until upstream
    // issue #67 is fixed.
    #[cfg(any(target_os = "ios", target_os = "macos"))]
    const BOOTSTRAP_UNKNOWN_SERVICE: std::ffi::c_int = 1102;

    // SAFETY: We have not spawned any other threads at this point.
    unsafe {
        platform::daemonize();
    }

    logging::init();

    let client_handle = unsafe { <OsString as BreakpadString>::from_ptr(client_handle) };
    let client_handle = unwrap_res_with_message(
        platform::get_client_handle(&client_handle),
        "Could not deserialize the client process handle",
    );
    let breakpad_data = BreakpadData::new(breakpad_data);
    let minidump_path = unsafe { <OsString as BreakpadString>::from_ptr(minidump_path) };
    let build_id = unsafe { <OsString as BreakpadString>::from_ptr(build_id) };
    let build_id =
        unwrap_option_with_message(build_id.to_str(), "BuildID is not a valid UTF-8 string")
            .to_string();
    initialize_static_annotations(&ApplicationInfo::new(build_id, client_handle.clone()));
    let listener = unsafe { <OsString as BreakpadString>::from_ptr(listener) };
    let listener = unwrap_res_with_message(
        IPCListener::deserialize(&listener, client_pid),
        "Could not parse the crash generator's listener",
    );
    let pipe = unsafe { <OsString as BreakpadString>::from_ptr(pipe) };
    let connector = IPCConnector::deserialize(&pipe);
    let connector = match connector {
        // If the main process went down before we could deserialize the
        // connector then deserialization will fail, handle this case as an
        // expected error rather than a panic.
        #[cfg(any(target_os = "ios", target_os = "macos"))]
        Err(crash_helper_common::errors::IPCError::Deserialize(
            crash_helper_common::PlatformError::BootstrapLookUp(_rv @ BOOTSTRAP_UNKNOWN_SERVICE),
        )) => {
            log::error!("Could not reach out to the main process, shutting down");
            return -1;
        }
        Err(e) => {
            log::error!("Could not deserialize connector: {e:?}");
            return -1;
        }
        Ok(connector) => connector,
    };

    let mode = if background_mode {
        ClientMode::Background
    } else {
        ClientMode::Foreground
    };

    let ipc_server = IPCServer::new(
        mode,
        client_pid,
        client_handle,
        listener,
        connector,
        breakpad_data,
        minidump_path,
    );

    let instance = CrashHelperInstance {
        mode,
    };

    match ipc_server {
        Ok(ipc_server) => instance.main_loop(ipc_server),
        Err(e) => {
            log::error!("Could not create the IPC server (error: {e:?})");
            #[cfg(not(target_os = "android"))]
            panic!("Could not create the IPC server (error: {e:?})");
            #[allow(unreachable_code)]
            -1
        }
    }
}

/// Runs the crash generator process logic, this includes the IPC used by
/// processes to signal that they crashed, the IPC used to retrieve crash
/// reports from the crash helper process and the logic used to generate the
/// actual minidumps. The logic will run in a separate thread and this
/// function will return immediately after launching it.
///
/// # Safety
///
/// `minidump_data` must point to valid, nul-terminated C strings. `server_pipe`
/// must be a valid file descriptor and `breakpad_data` must also be a valid
/// file descriptor compatible with Breakpad's crash generation server.
#[cfg(target_os = "android")]
#[no_mangle]
pub unsafe extern "C" fn crash_generator_logic_android(
    build_id: *const BreakpadChar,
    pid: Pid,
    crash_handler: *const BreakpadChar,
    breakpad_data: BreakpadRawData,
    minidump_path: *const BreakpadChar,
    pipe: crash_helper_common::RawIPCConnector,
) {
    use crash_helper_common::{FromRawProcessHandle, ProcessHandle};

    logging::init();

    let build_id = unsafe { <OsString as BreakpadString>::from_ptr(build_id) };
    initialize_static_annotations(&ApplicationInfo::new(
        build_id.to_str().unwrap().to_string(),
        Some(ProcessHandle::from_raw_handle(pid)),
    ));

    let crash_handler = unsafe { <OsString as BreakpadString>::from_ptr(crash_handler) };
    let crash_handler = crash_handler.to_str().unwrap().to_string();
    let breakpad_data = BreakpadData::new(breakpad_data);
    let minidump_path = unsafe { <OsString as BreakpadString>::from_ptr(minidump_path) };

    // On Android the main thread is used to respond to the intents so we
    // can't block it. Run the crash generation loop in a separate thread.
    let _ = std::thread::spawn(move || {
        let listener = IPCListener::new(0).expect("IPCListener creation failed!");
        // SAFETY: The `pipe` file descriptor passed in from the caller is
        // guaranteed to be valid.
        let connector = match unsafe { IPCConnector::from_raw_connector(pipe) } {
            Ok(connector) => connector,
            Err(error) => {
                log::error!("Could not use the pipe (error: {error})");
                return;
            }
        };
        let ipc_server = match IPCServer::new(
            ClientMode::Foreground,
            pid,
            /* client_handle */ None,
            listener,
            connector,
            breakpad_data,
            minidump_path,
        ) {
            Ok(ipc_server) => ipc_server,
            Err(error) => {
                log::error!("Could not create the IPC server (error: {error})");
                return;
            }
        };

        let instance = CrashHelperInstance {
            mode: ClientMode::Foreground,
            crash_handler,
        };

        instance.main_loop(ipc_server);
    });
}

struct CrashHelperInstance {
    mode: ClientMode,
    #[cfg(target_os = "android")]
    crash_handler: String,
}

impl CrashHelperInstance {
    fn main_loop(&self, mut ipc_server: IPCServer) -> i32 {
        loop {
            match ipc_server.run() {
                Ok(result) => {
                    match result {
                        IPCServerState::Running => {} // Go on
                        IPCServerState::ClientDisconnected => return 0,
                        IPCServerState::ClientCrashed(minidump, environment) => {
                            self.maybe_report_crash(&minidump, environment);
                            return 0;
                        }
                    }
                }
                Err(error) => {
                    log::error!("The crashhelper encountered an error, exiting (error: {error})");
                    return -1;
                }
            }
        }
    }

    fn maybe_report_crash(&self, minidump: &Path, environment: ClientEnvironment) {
        log::error!("maybe_report_crash()");
        if env_var_is_set("MOZ_CRASHREPORTER_NO_REPORT")
            || env_var_is_set("MOZ_CRASHREPORTER_FULLDUMP")
            || matches!(self.mode, ClientMode::Background)
        {
            return;
        }

        log::error!("self.report_crash() minidump = {}", minidump.display());
        self.report_crash(minidump, environment);
    }

    fn report_crash(&self, minidump: &Path, #[allow(unused)] environment: ClientEnvironment) {
        #[cfg(not(target_os = "android"))]
        {
            let crash_reporter = crashreporter_path();
            let _ = Command::new(crash_reporter.as_path())
                .arg(minidump)
                .envs(environment.into_env_vars())
                .spawn()
                .expect("Failed to launch the crash reporter client")
                .wait();
        }
        #[cfg(target_os = "android")]
        {
            use std::{ffi::CString, fs, os::unix::ffi::OsStrExt};

use nix::{libc::exit, unistd::{ForkResult, fork}};

            log::error!("self.report_crash()");
            let mut extra = PathBuf::from(minidump);
            extra.set_extension("extra");
            let serial_number = env::var("MOZ_ANDROID_USER_SERIAL_NUMBER");
            log::error!("serial_number = {serial_number:?}");

            let command = c"/system/bin/am";
            let mut args = vec![
                c"/system/bin/am",
                c"start-foreground-service",
            ];

            let serial_number = serial_number.map(|s| CString::new(s.as_bytes()).unwrap());

            if let Ok(ref serial_number) = serial_number {
                args.push(c"--user");
                args.push(serial_number);
            }
                args.push(c"--user");
                args.push(c"0");

            let crash_handler = CString::new(self.crash_handler.clone()).unwrap();
            let minidump = CString::new(minidump.as_os_str().as_bytes()).unwrap();
            let extra = CString::new(extra.as_os_str().as_bytes()).unwrap();

            args.extend(
                [
                    c"-a",
                    c"org.mozilla.gecko.ACTION_CRASHED",
                    c"-n",
                    &crash_handler,
                    c"--es",
                    c"minidumpPath",
                    &minidump,
                    c"--es",
                    c"extrasPath",
                    &extra,
                    c"--ex",
                    c"fatal",
                    c"true",
                    c"--es",
                    c"processVisibility",
                    c"MAIN",
                    c"--es",
                    c"processType",
                    c"main",
                ]
            );

            log::error!("args = {args:?}");
            let metadata = fs::metadata("/system/bin/am");
            log::error!("metadata = {metadata:?}");

            let res = unsafe { fork() };
            if let Ok(res) = res {
                match res {                ForkResult::Child => {
                    let res = nix::unistd::execv(&command, &args);
                },
                ForkResult::Parent{child} => {
                    use nix::sys::wait::{WaitPidFlag, waitpid};

                    log::error!("child = {child:?}");
                    let res = waitpid(child, Some(WaitPidFlag::__WALL));
                    log::error!("waitpid = {res:?}");
                }
}

            }

            log::error!("res = {res:?}");
            unsafe { exit(1) };
        }
    }
}

#[cfg(not(target_os = "android"))]
fn crashreporter_path() -> std::path::PathBuf {
    let mut path = unwrap_res_with_message(env::current_exe(), "Could not get the executable path");
    path.pop();
    path.push(CRASH_REPORTER_PATH);
    path
}

fn env_var_is_set(name: &'static str) -> bool {
    env::var(name).is_ok_and(|v| !v.is_empty())
}

#[cfg(not(target_os = "android"))]
fn unwrap_res_with_message<T, E: std::fmt::Display>(res: Result<T, E>, error_string: &str) -> T {
    match res {
        Ok(value) => value,
        Err(error) => {
            log::error!("{error_string} (error: {error})");
            panic!("{} (error: {})", error_string, error);
        }
    }
}

#[cfg(not(target_os = "android"))]
fn unwrap_option_with_message<T>(res: Option<T>, error_string: &str) -> T {
    match res {
        Some(value) => value,
        None => {
            log::error!("{error_string}");
            panic!("{}", error_string);
        }
    }
}
