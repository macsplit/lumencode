#include "geometry.h"

double summarize(double radius)
{
    return circleArea(radius) * 2;
}

int gridCells()
{
    Grid grid;
    return grid.cellCount(4, 5);
}
