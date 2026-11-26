/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

use anyhow::{bail, Result};
use crash_helper_common::{
    messages::ProcessRendezVous, BreakpadChar, BreakpadData, BreakpadString, GeckoChildId,
    IPCChannel, IPCConnector, IPCListener, Pid, ProcessHandle,
};
use std::{
    ffi::{c_char, CStr, OsStr, OsString},
    mem::{size_of, zeroed},
    os::windows::{
        ffi::{OsStrExt, OsStringExt},
        io::{AsHandle, BorrowedHandle, FromRawHandle, OwnedHandle, RawHandle},
    },
    ptr::{null, null_mut},
};
use windows_sys::Win32::{
    Foundation::{CloseHandle, GetLastError, ERROR_INSUFFICIENT_BUFFER, FALSE, HANDLE, TRUE},
    System::Threading::{
        CreateProcessW, DeleteProcThreadAttributeList, GetCurrentProcessId,
        InitializeProcThreadAttributeList, UpdateProcThreadAttribute, CREATE_UNICODE_ENVIRONMENT,
        DETACHED_PROCESS, EXTENDED_STARTUPINFO_PRESENT, LPPROC_THREAD_ATTRIBUTE_LIST,
        PROCESS_INFORMATION, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, STARTUPINFOEXW, STARTUPINFOW,
    },
};

use crate::CrashHelperClient;

struct ProcessThreadAttributeListStorage {
    storage: Vec<u8>,
}

impl ProcessThreadAttributeListStorage {
    fn new(n_attrs: u32) -> Result<ProcessThreadAttributeListStorage> {
        let mut size: usize = 0;
        // SAFETY: `size` is allocated on the stack and thus a pointer
        // to it is valid for the lifetime of the call.
        let res = unsafe {
            InitializeProcThreadAttributeList(null_mut(), n_attrs, /* dwflags */ 0, &mut size)
        };

        if (res != FALSE) || unsafe { GetLastError() != ERROR_INSUFFICIENT_BUFFER } {
            bail!("Could not compute the size of the attribute list");
        }

        let mut storage = vec![0u8; size];
        // SAFETY: The `storage` vector is allocated on the heap and
        // sized appropriately. `size` is also allocated on the heap
        // and thus valid for the duration of the call.
        let res = unsafe {
            InitializeProcThreadAttributeList(
                storage.as_mut_ptr() as LPPROC_THREAD_ATTRIBUTE_LIST,
                n_attrs,
                /* dwflags */ 0,
                &mut size,
            )
        };

        if res == FALSE {
            bail!("Could not initialize attribute list, error: {}", unsafe {
                GetLastError()
            });
        }

        Ok(ProcessThreadAttributeListStorage { storage })
    }

    fn as_mut_ptr(&mut self) -> LPPROC_THREAD_ATTRIBUTE_LIST {
        self.storage.as_mut_ptr() as _
    }
}

impl Drop for ProcessThreadAttributeListStorage {
    fn drop(&mut self) {
        // SAFETY: The `storage` field always contains an object that was
        // allocated using `InitializeProcThreadAttributeList()`.
        unsafe {
            DeleteProcThreadAttributeList(self.storage.as_mut_ptr() as LPPROC_THREAD_ATTRIBUTE_LIST);
        }
    }
}

struct ProcessThreadAttributeList<'a> {
    storage: ProcessThreadAttributeListStorage,
    #[allow(unused)]
    // This stores the buffer we pass to `UpdateProcThreadAttribute()` which
    // needs to remain valid at least until we dispose of the
    // `PROC_THREAD_ATTRIBUTE_LIST` object.
    handles: Vec<BorrowedHandle<'a>>,
}

impl<'a> ProcessThreadAttributeList<'a> {
    // This should be borrowed handles
    fn new(handles_ref: &'a [BorrowedHandle]) -> Result<ProcessThreadAttributeList<'a>> {
        let mut storage = ProcessThreadAttributeListStorage::new(1)?;
        let handles = Vec::from(handles_ref);

        // SAFETY: The `storage` and `handles` pointers both point to objects we
        // just allocated on the heap and which are guaranteed to be valid for the
        // duration of the call.
        let res = unsafe {
            UpdateProcThreadAttribute(
                storage.as_mut_ptr(),
                /* dwflags */ 0,
                PROC_THREAD_ATTRIBUTE_HANDLE_LIST as usize,
                handles.as_ptr() as _,
                handles.len() * size_of::<HANDLE>(),
                null_mut(),
                null_mut(),
            )
        };

        if res == FALSE {
            bail!(
                "Could not add the inherited handles to the attribute list, error: {}",
                unsafe { GetLastError() }
            );
        }

        Ok(ProcessThreadAttributeList { storage, handles })
    }

    fn as_mut_ptr(&mut self) -> LPPROC_THREAD_ATTRIBUTE_LIST {
        self.storage.as_mut_ptr()
    }
}

impl CrashHelperClient {
    pub(crate) fn new(
        program: *const BreakpadChar,
        breakpad_data: BreakpadData,
        minidump_path: *const BreakpadChar,
        build_id: *const c_char,
    ) -> Result<CrashHelperClient> {
        // SAFETY: `program` points to a valid string passed in by Firefox
        let program = unsafe { <OsString as BreakpadString>::from_ptr(program) };
        // SAFETY: `minidump_path` points to a valid string passed in by Firefox
        let minidump_path = unsafe { <OsString as BreakpadString>::from_ptr(minidump_path) };
        // SAFETY: `build_id` is guaranteed to point to a valid nul-terminated
        // string by the caller.
        let build_id = unsafe { CStr::from_ptr(build_id) };
        let build_id = OsString::from_wide(
            build_id
                .to_bytes()
                .iter()
                .map(|&c| c as u16)
                .collect::<Vec<u16>>()
                .as_ref(),
        );

        let channel = IPCChannel::new()?;
        let (listener, server_endpoint, client_endpoint) = channel.deconstruct();

        let _spawner_thread = std::thread::spawn(move || {
            CrashHelperClient::spawn_crash_helper(
                program,
                breakpad_data,
                minidump_path,
                server_endpoint,
                build_id,
                listener,
            )
        });

        Ok(CrashHelperClient {
            connector: client_endpoint,
            pid: 0, // Unused on Windows
        })
    }

    fn spawn_crash_helper(
        program: OsString,
        breakpad_data: BreakpadData,
        minidump_path: OsString,
        endpoint: IPCConnector,
        build_id: OsString,
        listener: IPCListener,
    ) -> Result<ProcessHandle> {
        // SAFETY: `GetCurrentProcessId()` takes no arguments and should always work
        let pid = OsString::from(unsafe { GetCurrentProcessId() }.to_string());
        let handle = ProcessHandle::current_process()?;

        let mut cmd_line = escape_cmd_line_arg(&program);
        cmd_line.push(" ");
        cmd_line.push(escape_cmd_line_arg(&pid));
        cmd_line.push(" ");
        cmd_line.push(escape_cmd_line_arg(&breakpad_data.serialize()));
        cmd_line.push(" ");
        cmd_line.push(escape_cmd_line_arg(&minidump_path));
        cmd_line.push(" ");
        cmd_line.push(escape_cmd_line_arg(&endpoint.serialize()?));
        cmd_line.push(" ");
        cmd_line.push(escape_cmd_line_arg(&build_id));
        cmd_line.push(" ");
        cmd_line.push(escape_cmd_line_arg(&listener.serialize()?));
        cmd_line.push(" ");
        cmd_line.push(escape_cmd_line_arg(&handle.serialize()?));
        cmd_line.push("\0");
        let mut cmd_line: Vec<u16> = cmd_line.encode_wide().collect();

        let handles = [
            breakpad_data.as_handle(),
            endpoint.as_handle(),
            listener.as_handle(),
            handle.as_handle(),
        ];
        let mut ptal = ProcessThreadAttributeList::new(&handles)?;
        let mut pi = unsafe { zeroed::<PROCESS_INFORMATION>() };
        let si = STARTUPINFOEXW {
            StartupInfo: STARTUPINFOW {
                cb: size_of::<STARTUPINFOEXW>().try_into().unwrap(),
                ..unsafe { zeroed() }
            },
            lpAttributeList: ptal.as_mut_ptr(),
            ..unsafe { zeroed() }
        };

        let res = unsafe {
            CreateProcessW(
                /* lpApplicationName */ null(),
                cmd_line.as_mut_ptr(),
                /* lpProcessAttributes */ null_mut(),
                /* lpThreadAttributes */ null_mut(),
                /* bInheritHandles */ TRUE,
                CREATE_UNICODE_ENVIRONMENT | DETACHED_PROCESS | EXTENDED_STARTUPINFO_PRESENT,
                /* lpEnvironment */ null_mut(),
                /* lpCurrentDirectory */ null_mut(),
                &si.StartupInfo,
                &mut pi,
            )
        };

        if res == FALSE {
            bail!("Could not create the crash helper process");
        }

        // SAFETY: We've verified that this value had been properly populated
        // in the lines above.
        unsafe {
            CloseHandle(pi.hThread);
        }

        // SAFETY: We've already checked that `pi.hProcess` contains a
        // valid process handle.
        Ok(ProcessHandle(unsafe {
            OwnedHandle::from_raw_handle(pi.hProcess as RawHandle)
        }))
    }

    pub(crate) fn prepare_for_minidump(
        _crash_helper_pid: Option<Pid>,
        _id: GeckoChildId,
    ) -> Option<ProcessRendezVous> {
        None
    }
}

/// Escape an argument so that it is suitable for use in the command line
/// parameter of a CreateProcess() call. This involves escaping all inner
/// double-quote characters as well as trailing backslashes. The algorithm
/// used is described in this article:
/// https://learn.microsoft.com/it-it/archive/blogs/twistylittlepassagesallalike/everyone-quotes-command-line-arguments-the-wrong-way
fn escape_cmd_line_arg(arg: &OsStr) -> OsString {
    const DOUBLE_QUOTES: u16 = '"' as u16;
    const BACKSLASH: u16 = '\\' as u16;

    let encoded_arg: Vec<u16> = arg.encode_wide().collect();
    let mut escaped_arg = Vec::<u16>::new();
    escaped_arg.push(DOUBLE_QUOTES);

    let mut it = encoded_arg.iter().peekable();
    loop {
        let mut backslash_num = 0;

        while let Some(&&_c @ BACKSLASH) = it.peek() {
            it.next();
            backslash_num += 1;
        }

        match it.peek() {
            None => {
                for _ in 0..backslash_num {
                    escaped_arg.extend([BACKSLASH, BACKSLASH]);
                }
                break;
            }
            Some(&&_c @ DOUBLE_QUOTES) => {
                for _ in 0..backslash_num {
                    escaped_arg.extend([BACKSLASH, BACKSLASH]);
                }
                escaped_arg.extend([BACKSLASH, DOUBLE_QUOTES]);
            }
            Some(&&c) => {
                escaped_arg.extend(std::iter::repeat_n(BACKSLASH, backslash_num));
                escaped_arg.push(c)
            }
        }

        it.next();
    }

    escaped_arg.push(DOUBLE_QUOTES);
    OsString::from_wide(&escaped_arg)
}
