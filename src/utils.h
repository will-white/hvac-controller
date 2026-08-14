#ifndef UTILS_H
#define UTILS_H

inline float fahrenheitToCelsius(float f) {
    return (f - 32.0) * 5.0 / 9.0;
}

inline float celsiusToFahrenheit(float c) {
    return (c * 9.0 / 5.0) + 32.0;
}

#endif // UTILS_H
