// Static data members live outside the object: they add no size, no offset
// and no vtable entries.
struct Counter {
  static int instances;
  char tag;
  static constexpr int limit = 8;
  int value;
  inline static long total = 0;
};

// Only static members: an empty class, size 1.
struct OnlyStatics {
  static int a;
  static double b;
};

// A static member may have the class's own, still incomplete, type.
struct Node {
  static Node sentinel;
  Node *next;
};

template <typename T> struct Registry {
  static T fallback;
  T item;
  static const T *head;
};
Registry<short> shorts;

// Static data and static member functions leave the vptr and entries alone.
struct Base {
  static int created;
  static Base *make();
  virtual int id() const;
  int x;
};
