#include <stdint.h>
#include <memory.h>
#include <math.h>
#include <fenv.h>
#include <new>
#include <thread>
#include "mandelbrot.hxx"
#include "shade.hxx"
#include "memBuffer.hxx"
#include "memory.hxx"

constexpr double power(double base, uint32_t exp) noexcept
	{ return exp == 0 ? 1 : base * power(base, exp - 1); }

constexpr static const double bailout = power(power(2, 8), 2);
static const double log_2 = log(2);

double computePoint(const point2_t p0) noexcept
{
	point2_t pp, p{};
	uint16_t iteration{0};

	for (; iteration < maxIterations; ++iteration)
	{
		pp = p * p;
		if (pp.sum() > bailout)
			break;
		p.y(2 * p.mul() + p0.y());
		p.x(pp.diff() + p0.x());
	}

	if (iteration < maxIterations && iteration)
	{
		pp = p * p;
		const double zn = log(pp.sum()) / 2;
		const double nu = log(zn / log_2) / log_2;
		feclearexcept(FE_ALL_EXCEPT);
		return iteration + 1 - nu;
	}

	return iteration;
}

void computeSubchunk(const area_t &size, const area_t &offset, const point2_t &scale,
	const point2_t &origin, memBuffer_t<rgb8_t> &buffer) noexcept
{
	puts("Subpixel worker launched");
	const uint32_t maxY = height - 1;
	for (uint32_t y{0}; y < size.height(); ++y)
	{
		for (uint32_t x{0}; x < size.width(); ++x)
		{
			area_t pixel = offset + area_t{x, y};
			pixel.height(maxY - pixel.height());
			const double iteration = computePoint((pixel / scale) + origin);
			buffer.write(shade(iteration));
		}
	}
	puts("Subpixel worker done");
}

void computeChunk(const area_t size, const area_t offset, const point2_t scale,
	const point2_t center, const uint32_t subdiv, stream_t &stream) noexcept try
{
	const point2_t origin = -((area_t{width, height} / scale) / 2) + center;
	const point2_t subpixelOrigin = -(point2_t{double(subdiv / 2), double(subdiv / 2)} / subdiv) / scale;
	const point2_t subpixelOffset = (point2_t{1, 1} / subdiv) / scale;
	const uint32_t totalSubdivs = subdiv * subdiv;
	memBuffer_t<rgb8_t>::length = width * height;
	auto subpixels = makeUnique<memBuffer_t<rgb8_t> []>(totalSubdivs);
	auto subchunkThreads = makeUnique<std::thread []>(totalSubdivs);
	if (!subpixels || !subchunkThreads)
		abort();

	printf("Launching %u subpixel workers\n", totalSubdivs);
	for (uint32_t y{0}; y < subdiv; ++y)
	{
		for (uint32_t x{0}; x < subdiv; ++x)
		{
			const point2_t subchunkOffset = (subpixelOffset * area_t{x, y}) + subpixelOrigin;
			const uint32_t index = x + (y * subdiv);
			subchunkThreads[index] = std::thread([&](const point2_t origin, const uint32_t index) noexcept
				{
					threadAffinity(index);
					computeSubchunk(size, offset, scale, origin, subpixels[index]);
				}, origin + subchunkOffset, index
			);
		}
	}

	fixedVector_t<rgb8_t> points{totalSubdivs};
	if (!points.valid())
		throw std::bad_alloc{};
	for (uint32_t y{0}; y < size.height(); ++y)
	{
		for (uint32_t x{0}; x < size.width(); ++x)
		{
			for (uint32_t i{0}; i < totalSubdivs; ++i)
				points[i] = subpixels[i].readNext();
			if (!write(stream, shadePixel(points)))
			{
				printf("Aborting at %u, %u - %s\n", x, y, strerror(errno));
				fflush(stdout);
				abort();
			}
		}
	}

	puts("Reaping subpixel workers");
	for (uint32_t i{0}; i < totalSubdivs; ++i)
		subchunkThreads[i].join();
}
catch (const std::bad_alloc &) { abort(); }
