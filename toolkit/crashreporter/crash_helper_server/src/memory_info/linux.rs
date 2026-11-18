/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

use crash_helper_common::crash_annotations::CrashAnnotation;
use mozannotation_server::CAnnotation;
use procfs_core::FromRead;

use crate::crash_generation::make_annotation_u64;

pub(crate) fn get_memory_info() -> Vec<CAnnotation> {
    let Ok(meminfo) = procfs_core::Meminfo::from_file("/proc/meminfo") else {
        // TODO: Also return errors and find a way of stuffing them in
        // the rest of the dumper error annotation.
        return vec![];
    };

    let mut annotations = Vec::with_capacity(6);
    let commit_limit = meminfo.commit_limit.unwrap_or(0);
    let available_page_file = commit_limit.saturating_sub(meminfo.committed_as);

    annotations.push(make_annotation_u64(
        CrashAnnotation::AvailablePageFile,
        available_page_file,
    ));
    annotations.push(make_annotation_u64(
        CrashAnnotation::AvailablePhysicalMemory,
        meminfo.mem_free,
    ));
    annotations.push(make_annotation_u64(
        CrashAnnotation::AvailableSwapMemory,
        meminfo.swap_free,
    ));

    if let Some(mem_available) = meminfo.mem_available {
        annotations.push(make_annotation_u64(
            CrashAnnotation::AvailableVirtualMemory,
            mem_available,
        ));
    }

    annotations.push(make_annotation_u64(
        CrashAnnotation::TotalPageFile,
        meminfo.mem_total.saturating_add(meminfo.swap_total),
    ));
    annotations.push(make_annotation_u64(
        CrashAnnotation::TotalPhysicalMemory,
        meminfo.mem_total,
    ));

    annotations
}
