macro(add_issue_test)
  add_simple_test(${ARGV} SOURCE_PREFIX "issue_" TARGET_PREFIX "test_issue_")
endmacro(add_issue_test)

add_issue_test(gh0028)
add_issue_test(gh0030)
add_issue_test(gh0033)
