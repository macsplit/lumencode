#include "geometry.h"

double circleArea(double radius)
{
    return 3.14159 * radius * radius;
}

int Grid::cellCount(int width, int height) const
{
    return width * height;
}
