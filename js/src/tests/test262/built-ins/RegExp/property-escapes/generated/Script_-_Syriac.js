// Copyright 2024 Mathias Bynens. All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.

/*---
author: Mathias Bynens
description: >
  Unicode property escapes for `Script=Syriac`
info: |
  Generated by https://github.com/mathiasbynens/unicode-property-escapes-tests
  Unicode v16.0.0
esid: sec-static-semantics-unicodematchproperty-p
features: [regexp-unicode-property-escapes]
includes: [regExpUtils.js]
---*/

const matchSymbols = buildString({
  loneCodePoints: [],
  ranges: [
    [0x000700, 0x00070D],
    [0x00070F, 0x00074A],
    [0x00074D, 0x00074F],
    [0x000860, 0x00086A]
  ]
});
testPropertyEscapes(
  /^\p{Script=Syriac}+$/u,
  matchSymbols,
  "\\p{Script=Syriac}"
);
testPropertyEscapes(
  /^\p{Script=Syrc}+$/u,
  matchSymbols,
  "\\p{Script=Syrc}"
);
testPropertyEscapes(
  /^\p{sc=Syriac}+$/u,
  matchSymbols,
  "\\p{sc=Syriac}"
);
testPropertyEscapes(
  /^\p{sc=Syrc}+$/u,
  matchSymbols,
  "\\p{sc=Syrc}"
);

const nonMatchSymbols = buildString({
  loneCodePoints: [
    0x00070E
  ],
  ranges: [
    [0x00DC00, 0x00DFFF],
    [0x000000, 0x0006FF],
    [0x00074B, 0x00074C],
    [0x000750, 0x00085F],
    [0x00086B, 0x00DBFF],
    [0x00E000, 0x10FFFF]
  ]
});
testPropertyEscapes(
  /^\P{Script=Syriac}+$/u,
  nonMatchSymbols,
  "\\P{Script=Syriac}"
);
testPropertyEscapes(
  /^\P{Script=Syrc}+$/u,
  nonMatchSymbols,
  "\\P{Script=Syrc}"
);
testPropertyEscapes(
  /^\P{sc=Syriac}+$/u,
  nonMatchSymbols,
  "\\P{sc=Syriac}"
);
testPropertyEscapes(
  /^\P{sc=Syrc}+$/u,
  nonMatchSymbols,
  "\\P{sc=Syrc}"
);

reportCompare(0, 0);
