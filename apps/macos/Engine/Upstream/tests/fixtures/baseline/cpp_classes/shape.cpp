#include "shape.h"
#include <cmath>

namespace geo {

Shape::Shape(std::string name) : name_(std::move(name)) {}

Circle::Circle(double radius) : Shape("circle"), radius_(radius) {}

double Circle::area() const
{
    return square(radius_) * M_PI;
}

static double square(double value)
{
    return value * value;
}

double totalArea(const Shape **shapes, int count)
{
    double total = 0.0;
    for (int i = 0; i < count; ++i) {
        total += shapes[i]->area();
    }
    return total;
}

} // namespace geo
