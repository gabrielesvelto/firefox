/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

module.exports = {
  plugins: [
    "@babel/plugin-proposal-class-properties",
    "@babel/plugin-proposal-async-generator-functions",
    "@babel/plugin-proposal-optional-chaining",
    "@babel/plugin-proposal-nullish-coalescing-operator",
    "@babel/plugin-transform-modules-commonjs",
    "babel-plugin-add-module-exports",
    "transform-amd-to-commonjs",
  ],
};
