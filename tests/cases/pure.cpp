// Abstract class with a pure virtual function.
struct Shape { virtual ~Shape() = default; virtual double area() const = 0; };
struct Square final : Shape { double area() const override; double side; };
