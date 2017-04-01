// Copyright 2017 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "Core/HW/DVD/DVDMath.h"

#include <cinttypes>
#include <cmath>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"

namespace DVDMath
{
// The size of the first Wii disc layer in bytes (2294912 sectors, 2048 bytes per sector)
constexpr u64 WII_DISC_LAYER_SIZE = 0x118240000;

// 24 mm
constexpr double DVD_INNER_RADIUS = 0.024;
// 58 mm
constexpr double WII_DVD_OUTER_RADIUS = 0.058;
// 38 mm
constexpr double GC_DVD_OUTER_RADIUS = 0.038;

// Approximate read speeds at the inner and outer locations of Wii and GC
// discs. These speeds are approximations of speeds measured on real Wiis.
constexpr double GC_DISC_INNER_READ_SPEED = 1024 * 1024 * 2.1;    // bytes/s
constexpr double GC_DISC_OUTER_READ_SPEED = 1024 * 1024 * 3.325;  // bytes/s
constexpr double WII_DISC_INNER_READ_SPEED = 1024 * 1024 * 3.48;  // bytes/s
constexpr double WII_DISC_OUTER_READ_SPEED = 1024 * 1024 * 8.41;  // bytes/s

// Experimentally measured seek constants. The time to seek appears to be
// linear, but short seeks appear to be lower velocity.
constexpr double SHORT_SEEK_MAX_DISTANCE = 0.001;   // 1 mm
constexpr double SHORT_SEEK_CONSTANT = 0.045;       // seconds
constexpr double SHORT_SEEK_VELOCITY_INVERSE = 50;  // inverse: s/m
constexpr double LONG_SEEK_CONSTANT = 0.085;        // seconds
constexpr double LONG_SEEK_VELOCITY_INVERSE = 4.5;  // inverse: s/m

// We can approximate the relationship between a byte offset on disc and its
// radial distance from the center by using an approximation for the length of
// a rolled material, which is the area of the material divided by the pitch
// (ie: assume that you can squish and deform the area of the disc into a
// rectangle as thick as the track pitch).
//
// In practice this yields good-enough numbers as a more exact formula
// involving the integral over a polar equation (too complex to describe here)
// or the approximation of a DVD as a set of concentric circles (which is a
// better approximation, but makes futher derivations more complicated than
// they need to be).
//
// From the area approximation, we end up with this formula:
//
// L = pi*(r.outer^2-r.inner^2)/pitch
//
// Where:
//   L = the data track's physical length
//   r.{inner,outer} = the inner/outer radii (24 mm and 58 mm)
//   pitch = the track pitch (.74 um)
//
// We can then use this equation to compute the radius for a given sector in
// the disc by mapping it along the length to a linear position and inverting
// the equation and solving for r.outer (using the DVD's r.inner and pitch)
// given that linear position:
//
// r.outer = sqrt(L * pitch / pi + r.inner^2)
//
// Where:
//   L = the offset's linear position, as offset/density
//   r.outer = the radius for the offset
//   r.inner and pitch are the same as before.
//
// The data density of the disc is just the number of bytes addressable on a
// DVD, divided by the spiral length holding that data. offset/density yields
// the linear position for a given offset.
//
// When we put it all together and simplify, we can compute the radius for a
// given byte offset as a drastically simplified:
//
// r = sqrt(offset/total_bytes*(r.outer^2-r.inner^2) + r.inner^2)
double CalculatePhysicalDiscPosition(u64 offset)
{
	// Just in case someone has an overly large disc image
	// that can't exist in reality...
	offset %= WII_DISC_LAYER_SIZE * 2;

	// Assumption: the layout on the second disc layer is opposite of the first,
	// ie layer 2 starts where layer 1 ends and goes backwards.
	if (offset > WII_DISC_LAYER_SIZE)
		offset = WII_DISC_LAYER_SIZE * 2 - offset;

	// The track pitch here is 0.74 um, but it cancels out and we don't need it

	// Note that because Wii and GC discs have identical data densities
	// we can simply use the Wii numbers in both cases
	return std::sqrt(
		static_cast<double>(offset) / WII_DISC_LAYER_SIZE *
		(WII_DVD_OUTER_RADIUS * WII_DVD_OUTER_RADIUS - DVD_INNER_RADIUS * DVD_INNER_RADIUS) +
		DVD_INNER_RADIUS * DVD_INNER_RADIUS);
}

// Returns the time in seconds to move the read head from one offset to
// another, plus the number of ticks to read one ECC block immediately
// afterwards. Based on hardware testing, this appears to be a function of the
// linear distance between the radius of the first and second positions on the
// disc, though the head speed varies depending on the length of the seek.
double CalculateSeekTime(u64 offset_from, u64 offset_to)
{
	const double position_from = CalculatePhysicalDiscPosition(offset_from);
	const double position_to = CalculatePhysicalDiscPosition(offset_to);

	// Seek time is roughly linear based on head distance travelled
	const double distance = fabs(position_from - position_to);

	if (distance < SHORT_SEEK_MAX_DISTANCE)
		return distance * SHORT_SEEK_VELOCITY_INVERSE + SHORT_SEEK_CONSTANT;
	else
		return distance * LONG_SEEK_VELOCITY_INVERSE + LONG_SEEK_CONSTANT;
}

// Returns the time in seconds it takes to read an amount of data from a disc,
// ignoring factors such as seek times. This is the streaming rate of the
// drive and varies between ~3-8MiB/s for Wii discs. Note that there is technically
// a DMA delay on top of this, but we model that as part of this read time.
double CalculateRawDiscReadTime(u64 offset, u64 length, bool wii_disc)
{
	// The Wii/GC have a CAV drive and the data has a constant pit length
	// regardless of location on disc. This means we can linearly interpolate
	// speed from the inner to outer radius. This matches a hardware test.
	// We're just picking a point halfway into the read as our benchmark for
	// read speed as speeds don't change materially in this small window.
	const double physical_offset = CalculatePhysicalDiscPosition(offset + length / 2);

	double speed;
	if (wii_disc)
	{
		speed = (physical_offset - DVD_INNER_RADIUS) / (WII_DVD_OUTER_RADIUS - DVD_INNER_RADIUS) *
			(WII_DISC_OUTER_READ_SPEED - WII_DISC_INNER_READ_SPEED) +
			WII_DISC_INNER_READ_SPEED;
	}
	else
	{
		speed = (physical_offset - DVD_INNER_RADIUS) / (GC_DVD_OUTER_RADIUS - DVD_INNER_RADIUS) *
			(GC_DISC_OUTER_READ_SPEED - GC_DISC_INNER_READ_SPEED) +
			GC_DISC_INNER_READ_SPEED;
	}

	DEBUG_LOG(DVDINTERFACE, "Read 0x%" PRIx64 " @ 0x%" PRIx64 " @%lf mm: %lf us, %lf MiB/s", length,
		offset, physical_offset * 1000, length / speed * 1000 * 1000, speed / 1024 / 1024);

	return length / speed;
}

}  // namespace DVDMath
