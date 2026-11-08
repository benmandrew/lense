// Unions, anonymous members, nested records and arrays.
struct Point { short x, y; };
union Value { char bytes[5]; int i; double d; };
struct Packet {
  char kind;
  union { int as_int; float as_float; };
  struct { char tag; long id; } header;
  Point points[2];
  Value v;
};
