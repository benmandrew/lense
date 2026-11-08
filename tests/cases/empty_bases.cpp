// Empty base optimisation and [[no_unique_address]].
struct Empty {};
struct Tag {};
struct WithEmptyBase : Empty { int x; };
struct TwoEmpty : Empty, Tag { char c; };
struct NoUnique { [[no_unique_address]] Empty e; int x; };
struct Holder { Empty e; int x; };
