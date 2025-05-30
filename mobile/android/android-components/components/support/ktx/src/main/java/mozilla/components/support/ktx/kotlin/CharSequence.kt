/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package mozilla.components.support.ktx.kotlin

import mozilla.components.support.base.utils.MAX_URI_LENGTH

/**
 * Returns a trimmed CharSequence. This is used to prevent extreme cases
 * from slowing down UI rendering with large strings.
 */
fun CharSequence.trimmed(): CharSequence {
    return this.take(MAX_URI_LENGTH)
}
