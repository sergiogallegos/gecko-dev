// Copyright 2024 Mathias Bynens. All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.

/*---
author: Mathias Bynens
description: >
  Unicode property escapes for `Script_Extensions=SignWriting`
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
    [0x01D800, 0x01DA8B],
    [0x01DA9B, 0x01DA9F],
    [0x01DAA1, 0x01DAAF]
  ]
});
testPropertyEscapes(
  /^\p{Script_Extensions=SignWriting}+$/u,
  matchSymbols,
  "\\p{Script_Extensions=SignWriting}"
);
testPropertyEscapes(
  /^\p{Script_Extensions=Sgnw}+$/u,
  matchSymbols,
  "\\p{Script_Extensions=Sgnw}"
);
testPropertyEscapes(
  /^\p{scx=SignWriting}+$/u,
  matchSymbols,
  "\\p{scx=SignWriting}"
);
testPropertyEscapes(
  /^\p{scx=Sgnw}+$/u,
  matchSymbols,
  "\\p{scx=Sgnw}"
);

const nonMatchSymbols = buildString({
  loneCodePoints: [
    0x01DAA0
  ],
  ranges: [
    [0x00DC00, 0x00DFFF],
    [0x000000, 0x00DBFF],
    [0x00E000, 0x01D7FF],
    [0x01DA8C, 0x01DA9A],
    [0x01DAB0, 0x10FFFF]
  ]
});
testPropertyEscapes(
  /^\P{Script_Extensions=SignWriting}+$/u,
  nonMatchSymbols,
  "\\P{Script_Extensions=SignWriting}"
);
testPropertyEscapes(
  /^\P{Script_Extensions=Sgnw}+$/u,
  nonMatchSymbols,
  "\\P{Script_Extensions=Sgnw}"
);
testPropertyEscapes(
  /^\P{scx=SignWriting}+$/u,
  nonMatchSymbols,
  "\\P{scx=SignWriting}"
);
testPropertyEscapes(
  /^\P{scx=Sgnw}+$/u,
  nonMatchSymbols,
  "\\P{scx=Sgnw}"
);

reportCompare(0, 0);
