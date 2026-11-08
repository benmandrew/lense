// Only instantiated templates have a layout.
template <typename T> struct Box { virtual ~Box() {} T value; };
template <typename T, int N> struct Array { T items[N]; };
namespace ns { struct Inner { Box<char> b; Array<short, 3> a; }; }
Box<double> instantiated;
