/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// On Windows Breakpad uses a named pipe for communication between the
// exception handlers and the crash generator, and Windows UTF-16 strings for
// name and paths, so all we need to pass between processes are
// 2-bytes-per-character strings.

use std::{ffi::OsString, os::windows::io::RawHandle, str::FromStr};

pub type Pid = u32;
pub type BreakpadChar = u16;
pub type BreakpadRawData = RawHandle;

pub struct BreakpadData {
    handle: RawHandle,
}

impl BreakpadData {
    /// Create a new instance of the BreakpadData object from a Windows HANDLE.
    ///
    /// # Safety
    ///
    /// `raw` must be a valid handle to a named pipe.
    pub unsafe fn new(raw: BreakpadRawData) -> BreakpadData {
        BreakpadData { handle: raw }
    }

    pub fn into_raw(self) -> BreakpadRawData {
        self.handle
    }

    pub fn serialize(&self) -> OsString {
        let raw_handle = self.handle as usize;
        OsString::from_str(raw_handle.to_string().as_ref()).unwrap()
    }
}

// SAFETY: The breakpad raw data can be transferred across threads provided
// it's only used in the receiving thread which the caller should enforce.
unsafe impl Send for BreakpadData {}
