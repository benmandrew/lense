// A compile error: lense exits 1 but still reports the valid record.
struct Ok { int a; };
struct Bad { Missing m; int b; };
