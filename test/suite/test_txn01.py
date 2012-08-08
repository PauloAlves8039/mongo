#!/usr/bin/env python
#
# Copyright (c) 2008-2012 WiredTiger, Inc.
#
# This is free and unencumbered software released into the public domain.
#
# Anyone is free to copy, modify, publish, use, compile, sell, or
# distribute this software, either in source code form or as a compiled
# binary, for any purpose, commercial or non-commercial, and by any
# means.
#
# In jurisdictions that recognize copyright laws, the author or authors
# of this software dedicate any and all copyright interest in the
# software to the public domain. We make this dedication for the benefit
# of the public at large and to the detriment of our heirs and
# successors. We intend this dedication to be an overt act of
# relinquishment in perpetuity of all present and future rights to this
# software under copyright law.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
# IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
# OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
# ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
# OTHER DEALINGS IN THE SOFTWARE.

import wiredtiger, wttest

# test_txn01.py
#    Transactions: basic functionality
class test_txn01(wttest.WiredTigerTestCase):
    nentries = 1000
    scenarios = [
        ('col', dict(uri='file:text_txn01',key_format='r',value_format='S')),
        ('col', dict(uri='table:text_txn01',key_format='r',value_format='S')),
        ('fix', dict(uri='file:text_txn01',key_format='r',value_format='8t')),
        ('fix', dict(uri='table:text_txn01',key_format='r',value_format='8t')),
        ('row', dict(uri='file:text_txn01',key_format='S',value_format='S')),
        ('row', dict(uri='table:text_txn01',key_format='S',value_format='S')),
    ]

    # Overrides WiredTigerTestCase
    def setUpConnectionOpen(self, dir):
        conn = wiredtiger.wiredtiger_open(dir, 'create,' +
                ('error_prefix="%s: ",' % self.shortid()) +
                'transactional,')
        self.pr(`conn`)
        return conn

    # Return the number of records visible to the cursor.
    def cursor_count(self, cursor):
        count = 0
        # Column-store appends result in phantoms, ignore records unless they
        # have our flag value.
        for r in cursor:
            if self.value_format == 'S' or cursor.get_value() == 0xab:
                count += 1
        return count

    # Checkpoint the database and assert the number of records visible to the
    # checkpoint matches the expected value.
    def check_checkpoint(self, expected):
        s = self.conn.open_session()
        s.checkpoint("name=test")
        cursor = s.open_cursor(self.uri, None, "checkpoint=test")
        self.assertEqual(self.cursor_count(cursor), expected)
        s.close()

    # Open a cursor with snapshot isolation, and assert the number of records
    # visible to the cursor matches the expected value.
    def check_transaction(self, level, expected):
        s = self.conn.open_session()
        s.begin_transaction(level)
        cursor = s.open_cursor(self.uri, None)
        self.assertEqual(self.cursor_count(cursor), expected)
        s.close()

    def check(self, committed, total):
        self.check_transaction('isolation=snapshot', committed)
        #self.check_transaction('isolation=read-committed', committed)
        self.check_transaction('isolation=read-uncommitted', total)
        self.check_checkpoint(committed)

    # Loop through a set of inserts, periodically committing; before each
    # commit, verify the number of visible records matches the expected value.
    def test_visibilty(self):
        self.session.create(self.uri,
            'key_format=' + self.key_format +
            ',value_format=' + self.value_format)
        self.check(0, 0)

        committed = 0
        self.session.begin_transaction()
        cursor = self.session.open_cursor(self.uri, None)
        for i in xrange(self.nentries):
            if i > 0 and i % (self.nentries / 37) == 0:
                self.check(committed, i)
                self.session.commit_transaction()
                committed = i
                self.session.begin_transaction()
                cursor = self.session.open_cursor(self.uri, None)

            if self.key_format == 'S':
                cursor.set_key("key: %06d" % i)
            else:
                cursor.set_key(i + 1)
            if self.value_format == 'S':
                cursor.set_value("value: %06d" % i)
            else:
                cursor.set_value(0xab)
            cursor.insert()

        self.check(committed, self.nentries)
        self.session.commit_transaction()
        self.check(self.nentries, self.nentries)

if __name__ == '__main__':
    wttest.run()
