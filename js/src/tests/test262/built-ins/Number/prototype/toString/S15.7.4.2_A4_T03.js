// Copyright 2009 the Sputnik authors.  All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.

/*---
info: |
    The toString function is not generic, it cannot be transferred
    to other kinds of objects for use as a method and there is should be
    a TypeError exception if its this value is not a Number object
es5id: 15.7.4.2_A4_T03
description: transferring to the Date objects
---*/

var s1 = new Date(0);
Object.defineProperty(s1, "toString", {value: Number.prototype.toString});
try {
  var v1 = s1.toString();
  throw new Test262Error('#1: Number.prototype.toString on not a Number object should throw TypeError');
}
catch (e) {
  assert(e instanceof TypeError, 'The result of evaluating (e instanceof TypeError) is expected to be true');
}

var s2 = new Date(0);
s2.myToString = Number.prototype.toString;
try {
  var v2 = s2.myToString();
  throw new Test262Error('#2: Number.prototype.toString on not a Number object should throw TypeError');
}
catch (e) {
  assert(e instanceof TypeError, 'The result of evaluating (e instanceof TypeError) is expected to be true');
}

reportCompare(0, 0);
