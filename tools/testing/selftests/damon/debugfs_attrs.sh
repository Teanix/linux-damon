#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

test_write_result() {
	file=$1
	content=$2
	orig_content=$3
	expect_reason=$4
	expected=$5

	echo "$content" > "$file"
	if [ $? -ne "$expected" ]
	then
		echo "writing $content to $file doesn't return $expected"
		echo "expected because: $expect_reason"
		echo "$orig_content" > "$file"
		exit 1
	fi
}

test_write_succ() {
	test_write_result "$1" "$2" "$3" "$4" 0
}

test_write_fail() {
	test_write_result "$1" "$2" "$3" "$4" 1
}

test_content() {
	file=$1
	orig_content=$2
	expected=$3
	expect_reason=$4

	content=$(cat "$file")
	if [ "$content" != "$content" ]
	then
		echo "reading $file expected $expected but $content"
		echo "expected because: $expect_reason"
		echo "$orig_content" > "$file"
		exit 1
	fi
}

source ./_chk_dependency.sh

# Test attrs file
# ===============

file="$DBGFS/attrs"
orig_content=$(cat "$file")

test_write_succ "$file" "1 2 3 4 5" "$orig_content" "valid input"
test_write_fail "$file" "1 2 3 4" "$orig_content" "no enough fields"
test_write_fail "$file" "1 2 3 5 4" "$orig_content" \
	"min_nr_regions > max_nr_regions"
test_content "$file" "$orig_content" "1 2 3 4 5" "successfully written"
echo "$orig_content" > "$file"

# Test record file
file="$DBGFS/record"

ORIG_CONTENT=$(cat $file)

echo "4242 foo.bar" > $file
if [ $? -ne 0 ]
then
	echo "$file writing sane input failed"
	echo $ORIG_CONTENT > $file
	exit 1
fi

echo abc 2 3 > $file
if [ $? -eq 0 ]
then
	echo "$file writing insane input 1 success (should failed)"
	echo $ORIG_CONTENT > $file
	exit 1
fi

echo 123 > $file
if [ $? -eq 0 ]
then
	echo "$file writing insane input 2 success (should failed)"
	echo $ORIG_CONTENT > $file
	exit 1
fi

CONTENT=$(cat $file)
if [ "$CONTENT" != "4242 foo.bar" ]
then
	echo "$file not written"
	echo $ORIG_CONTENT > $file
	exit 1
fi

echo "0 null" > $file
if [ $? -ne 0 ]
then
	echo "$file disabling write fail"
	echo $ORIG_CONTENT > $file
	exit 1
fi

CONTENT=$(cat $file)
if [ "$CONTENT" != "0 null" ]
then
	echo "$file not disabled"
	echo $ORIG_CONTENT > $file
	exit 1
fi

echo "4242 foo.bar" > $file
if [ $? -ne 0 ]
then
	echo "$file writing sane data again fail"
	echo $ORIG_CONTENT > $file
	exit 1
fi

echo $ORIG_CONTENT > $file

# Test target_ids file
# ====================

file="$DBGFS/target_ids"
orig_content=$(cat "$file")

test_write_succ "$file" "1 2 3 4" "$orig_content" "valid input"
test_write_succ "$file" "1 2 abc 4" "$orig_content" "still valid input"
test_content "$file" "$orig_content" "1 2" "non-integer was there"
test_write_succ "$file" "abc 2 3" "$orig_content" "the file allows wrong input"
test_content "$file" "$orig_content" "" "wrong input written"
test_write_succ "$file" "" "$orig_content" "empty input"
test_content "$file" "$orig_content" "" "empty input written"
echo "$orig_content" > "$file"

echo "PASS"
