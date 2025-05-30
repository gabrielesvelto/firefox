// |reftest| shell-option(--enable-upsert) skip-if(!Map.prototype.getOrInsertComputed||!xulRuntime.shell) -- upsert is not enabled unconditionally, requires shell-options
// Copyright (C) 2015 the V8 project authors. All rights reserved.
// Copyright (C) 2024 Jonas Haukenes. All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.
/*---
esid: sec-map.prototype.getorinsert
description: |
  Map.prototype.getOrInsert.name value and descriptor.
info: |
  Map.prototype.getOrInsert ( key , value )

  17 ECMAScript Standard Built-in Objects
includes: [propertyHelper.js]
features: [upsert]
---*/
verifyProperty(Map.prototype.getOrInsert, "name", {
  value: "getOrInsert",
  writable: false,
  enumerable: false,
  configurable: true
});


reportCompare(0, 0);
