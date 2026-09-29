#pragma once
#include <string>

namespace geo {

enum class Kind { Circle, Square };

class Shape {
public:
    explicit Shape(std::string name);
    virtual ~Shape() = default;
    virtual double area() const = 0;
    const std::string &name() const { return name_; }

protected:
    std::string name_;
};

class Circle : public Shape {
public:
    Circle(double radius);
    double area() const override;

private:
    double radius_;
};

double totalArea(const Shape **shapes, int count);

} // namespace geo
