// |reftest| shell-option(--enable-regexp-modifiers) skip-if(release_or_beta||!xulRuntime.shell) -- regexp-modifiers is not released yet, requires shell-options
// Copyright 2023 Ron Buckton. All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.

/*---
author: Ron Buckton
description: >
  ignoreCase (`i`) modifier can be removed via `(?i:)` or `(?i-:)`.
info: |
  Runtime Semantics: CompileAtom
  The syntax-directed operation CompileAtom takes arguments direction (forward or backward) and modifiers (a Modifiers Record) and returns a Matcher.

  Atom :: `(` `?` RegularExpressionFlags `-` RegularExpressionFlags `:` Disjunction `)`
    1. Let addModifiers be the source text matched by the first RegularExpressionFlags.
    2. Let removeModifiers be the source text matched by the second RegularExpressionFlags.
    3. Let newModifiers be UpdateModifiers(modifiers, CodePointsToString(addModifiers), CodePointsToString(removeModifiers)).
    4. Return CompileSubpattern of Disjunction with arguments direction and newModifiers.

  UpdateModifiers ( modifiers, add, remove )
  The abstract operation UpdateModifiers takes arguments modifiers (a Modifiers Record), add (a String), and remove (a String) and returns a Modifiers. It performs the following steps when called:

  1. Let dotAll be modifiers.[[DotAll]].
  2. Let ignoreCase be modifiers.[[IgnoreCase]].
  3. Let multiline be modifiers.[[Multiline]].
  4. If add contains "s", set dotAll to true.
  5. If add contains "i", set ignoreCase to true.
  6. If add contains "m", set multiline to true.
  7. If remove contains "s", set dotAll to false.
  8. If remove contains "i", set ignoreCase to false.
  9. If remove contains "m", set multiline to false.
  10. Return the Modifiers Record { [[DotAll]]: dotAll, [[IgnoreCase]]: ignoreCase, [[Multiline]]: multiline }.

esid: sec-compileatom
features: [regexp-modifiers]
---*/

var re1 = /(?-i:fo)o/i;
assert(!re1.test("FOO"), "Pattern should not match as modified group does not ignore case");
assert(!re1.test("FOo"), "Pattern should not match as modified group does not ignore case");
assert(re1.test("foo"), "Pattern should not ignore case in modified group");
assert(re1.test("foO"), "Pattern should not ignore case in modified group");

var re2 = new RegExp("(?-i:fo)o", "i");
assert(!re2.test("FOO"), "Pattern should not match as modified group does not ignore case");
assert(!re2.test("FOo"), "Pattern should not match as modified group does not ignore case");
assert(re2.test("foo"), "Pattern should not ignore case in modified group");
assert(re2.test("foO"), "Pattern should not ignore case in modified group");

reportCompare(0, 0);
