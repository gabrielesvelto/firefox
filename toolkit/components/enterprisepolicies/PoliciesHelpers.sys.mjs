/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

const lazy = {};

const PREF_LOGLEVEL = "browser.policies.loglevel";

ChromeUtils.defineLazyGetter(lazy, "log", () => {
  let { ConsoleAPI } = ChromeUtils.importESModule(
    "resource://gre/modules/Console.sys.mjs"
  );
  return new ConsoleAPI({
    prefix: "Policies",
    // tip: set maxLogLevel to "debug" and use log.debug() to create detailed
    // messages during development. See LOG_LEVELS in Console.sys.mjs for details.
    maxLogLevel: "warn",
    maxLogLevelPref: PREF_LOGLEVEL,
  });
});

/**
 * Records the operations that failed while a policy was being applied, so that
 * about:policies can report that policy as only partially applied.
 *
 * A policy whose operation failed stays in getActivePolicies(): those are the
 * parameters the administrator asked for, and most of the policy may well have
 * been applied. What failed is recorded here instead.
 */
export const PolicyFailures = {
  /** @type {Map<string, string[]>} */
  _failures: new Map(),

  /**
   * Records a failed operation of a policy. Callers log the message
   * themselves, with their own logger.
   *
   * @param {string} policyName
   *        The policy the failed operation belongs to.
   * @param {string} message
   *        A description of what failed.
   */
  report(policyName, message) {
    let messages = this._failures.get(policyName);
    if (!messages) {
      messages = [];
      this._failures.set(policyName, messages);
    }
    if (!messages.includes(message)) {
      messages.push(message);
    }
  },

  /**
   * @returns {object}
   *          The failures of every policy, keyed by policy name.
   */
  getAll() {
    const failures = {};
    for (const [policyName, messages] of this._failures) {
      failures[policyName] = [...messages];
    }
    return failures;
  },

  /**
   * Forgets the failures of a policy, which happens when the policy is
   * applied again or removed.
   *
   * @param {string} policyName policy name
   */
  clear(policyName) {
    this._failures.delete(policyName);
  },

  clearAll() {
    this._failures.clear();
  },
};

/**
 * Logs a failed operation of a policy and records it against that policy, so
 * that about:policies reports the policy as only partially applied. Add-on
 * installations happen asynchronously, long after the policy callback
 * returned, so their failures have to be reported here explicitly to be
 * visible at all.
 *
 * @param {string} policyName
 *        The policy the failed operation belongs to. When it isn't known, the
 *        failure is only logged.
 * @param {string} message
 *        A description of what failed.
 */
export function reportFailure(policyName, message) {
  lazy.log.error(message);
  if (policyName) {
    PolicyFailures.report(policyName, message);
  }
}
