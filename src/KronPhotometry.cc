// -*- LSST-C++ -*-
#include <numeric>
#include <cmath>
#include <functional>
#include "boost/math/constants/constants.hpp"
#include "lsst/pex/exceptions.h"
#include "lsst/afw/geom/Point.h"
#include "lsst/afw/geom/Box.h"
#include "lsst/afw/image/Exposure.h"
#include "lsst/afw/table/Source.h"
#include "lsst/afw/math/Integrate.h"
#include "lsst/afw/math/FunctionLibrary.h"
#include "lsst/afw/math/KernelFunctions.h"
#include "lsst/meas/algorithms/Measure.h"
#include "lsst/afw/detection/Psf.h"
#include "lsst/afw/coord/Coord.h"
#include "lsst/afw/geom/AffineTransform.h"
#include "lsst/afw/geom/ellipses.h"
#include "lsst/meas/algorithms/Photometry.h"
#include "lsst/meas/algorithms/PSF.h"

#include "lsst/meas/extensions/photometryKron.h"
#include "lsst/meas/algorithms/ScaledFlux.h"

namespace lsst {
namespace meas {
namespace extensions {
namespace photometryKron {

LSST_EXCEPTION_TYPE(BadKronException, pex::exceptions::RuntimeError,
                    lsst::meas::extensions::photometryKron::BadKronException);

namespace {

template <typename MaskedImageT>
        class FootprintFlux : public afw::detection::FootprintFunctor<MaskedImageT> {
public:
    explicit FootprintFlux(MaskedImageT const& mimage ///< The image the source lives in
                          ) : afw::detection::FootprintFunctor<MaskedImageT>(mimage),
                     _sum(0.0), _sumVar(0.0) {}

    /// @brief Reset everything for a new Footprint
    void reset() {
        _sum = _sumVar = 0.0;
    }
    void reset(afw::detection::Footprint const&) {}        

    /// @brief method called for each pixel by apply()
    void operator()(typename MaskedImageT::xy_locator loc, ///< locator pointing at the pixel
                    int,                                   ///< column-position of pixel
                    int                                    ///< row-position of pixel
                   ) {
        typename MaskedImageT::Image::Pixel ival = loc.image(0, 0);
        typename MaskedImageT::Variance::Pixel vval = loc.variance(0, 0);
        _sum += ival;
        _sumVar += vval;
    }

    /// Return the Footprint's flux
    double getSum() const { return _sum; }

    /// Return the variance of the Footprint's flux
    double getSumVar() const { return _sumVar; }

private:
    double _sum;
    double _sumVar;
};

struct KronAperture;

/**
 * @brief A class that knows how to calculate fluxes using the KRON photometry algorithm
 *
 * @ingroup meas/algorithms
 */
class KronFlux : public algorithms::FluxAlgorithm, public algorithms::ScaledFlux {
public:

    KronFlux(KronFluxControl const & ctrl, afw::table::Schema & schema) :
        algorithms::FluxAlgorithm(
            ctrl, schema,
            "Kron photometry: photometry with aperture set to some multiple of <radius>"
            "determined within some multiple of the source size"
        ),
        _fluxCorrectionKeys(ctrl.name, schema),
        _radiusKey(schema.addField<float>(ctrl.name + ".radius", "Kron radius (sqrt(a*b))")),
        _radiusForRadiusKey(schema.addField<float>(ctrl.name + ".radiusForRadius",
                                          "Radius used to estimate <radius> (sqrt(a*b))")),
        _edgeKey(schema.addField<afw::table::Flag>(ctrl.name + ".flags.edge",
                                                   "Inaccurate measurement due to image edge")),
        _badRadiusKey(schema.addField<afw::table::Flag>(ctrl.name + ".flags.radius",
                                                        "Bad Kron radius")),
        _smallRadiusKey(schema.addField<afw::table::Flag>(ctrl.name + ".flags.smallRadius",
                                                     "Measured Kron radius was smaller than that of the PSF")),
        _usedMinimumRadiusKey(schema.addField<afw::table::Flag>(ctrl.name + ".flags.usedMinimumRadius",
                                                             "Used the minimum radius for the Kron aperture")),
        _usedPsfRadiusKey(schema.addField<afw::table::Flag>(ctrl.name + ".flags.usedPsfRadius",
                                                            "Used the PSF Kron radius for the Kron aperture")),
        _psfRadiusKey(schema.addField<float>(ctrl.name + ".psfRadius", "Radius of PSF")),
        _badShapeKey(schema.addField<afw::table::Flag>(ctrl.name + ".flags.badShape",
                                                    "Shape for measuring Kron radius is bad; used PSF shape"))
    {}


    virtual afw::table::KeyTuple<afw::table::Flux> getFluxKeys(int n=0) const {
        return FluxAlgorithm::getKeys();
    }

    virtual algorithms::ScaledFlux::KeyTuple getFluxCorrectionKeys(int n=0) const {
        return _fluxCorrectionKeys;
    }

private:

    template <typename PixelT>
    void _applyAperture(
        afw::table::SourceRecord & source,
        afw::image::Exposure<PixelT> const& exposure,
        KronAperture const& aperture
        ) const;

    template <typename PixelT>
    void _apply(
        afw::table::SourceRecord & source,
        afw::image::Exposure<PixelT> const & exposure,
        afw::geom::Point2D const & center
    ) const;

    template <typename PixelT>
    void _applyForced(
        afw::table::SourceRecord & source,
        afw::image::Exposure<PixelT> const & exposure,
        afw::geom::Point2D const & center,
        afw::table::SourceRecord const & reference,
        afw::geom::AffineTransform const & refToMeas
    ) const;

    // Provide a KronAperture for when the regular aperture fails
    //
    // Essentially this is an error handler that provides a fallback if possible,
    // or re-throws the exception.
    //
    // The fallback aperture uses the minimumRadius if defined; otherwise uses the PSF Kron radius.
    PTR(KronAperture) _fallbackRadius(afw::table::SourceRecord& source, // Source, to set flags
                                      double const R_K_psf,             // Kron radius of PSF
                                      pex::exceptions::Exception& exc   // Exception to re-throw, if required
        ) const;

    LSST_MEAS_ALGORITHM_PRIVATE_INTERFACE(KronFlux);

    algorithms::ScaledFlux::KeyTuple _fluxCorrectionKeys;
    afw::table::Key<float> _radiusKey;
    afw::table::Key<float> _radiusForRadiusKey;
    afw::table::Key<afw::table::Flag> _edgeKey;
    afw::table::Key<afw::table::Flag> _badRadiusKey;
    afw::table::Key<afw::table::Flag> _smallRadiusKey;
    afw::table::Key<afw::table::Flag> _usedMinimumRadiusKey;
    afw::table::Key<afw::table::Flag> _usedPsfRadiusKey;
    afw::table::Key<float> _psfRadiusKey;
    afw::table::Key<afw::table::Flag> _badShapeKey;
};

/************************************************************************************************************/
///
/// Find the first elliptical moment of an object
///
/// We know the shape and orientation of the ellipse to use, specified by ab and theta
/// If we take theta to be 0 the major axis is aligned along the x axis.  In this case
/// we may define the elliptical radius as sqrt(x^2 + (y*a/b)^2) of a point (x, y).
///
/// In other words, it's the length of the major axis of the ellipse of specified shape that passes through
/// the point
///
template <typename MaskedImageT, typename WeightImageT>
class FootprintFindMoment : public afw::detection::FootprintFunctor<MaskedImageT> {
public:
    FootprintFindMoment(MaskedImageT const& mimage, ///< The image the source lives in
                        afw::geom::Point2D const& center, // center of the object
                        double const ab,                // axis ratio
                        double const theta // rotation of ellipse +ve from x axis
        ) : afw::detection::FootprintFunctor<MaskedImageT>(mimage),
                           _xcen(center.getX()), _ycen(center.getY()),
                           _ab(ab),
                           _cosTheta(::cos(theta)),
                           _sinTheta(::sin(theta)),
                           _sum(0.0), _sumR(0.0), 
#if 0
                           _sumVar(0.0), _sumRVar(0.0),
#endif
                           _imageX0(mimage.getX0()), _imageY0(mimage.getY0())
        {}
    
    /// @brief Reset everything for a new Footprint
    void reset() {}        
    void reset(afw::detection::Footprint const& foot) {
        _sum = _sumR = 0.0;
#if 0
        _sumVar = _sumRVar = 0.0;
#endif

        MaskedImageT const& mimage = this->getImage();
        afw::geom::Box2I const& bbox(foot.getBBox());
        int const x0 = bbox.getMinX(), y0 = bbox.getMinY(), x1 = bbox.getMaxX(), y1 = bbox.getMaxY();

        if (x0 < _imageX0 || y0 < _imageY0 ||
            x1 >= _imageX0 + mimage.getWidth() || y1 >= _imageY0 + mimage.getHeight()) {
            throw LSST_EXCEPT(lsst::pex::exceptions::OutOfRangeError,
                              (boost::format("Footprint %d,%d--%d,%d doesn't fit in image %d,%d--%d,%d")
                               % x0 % y0 % x1 % y1
                               % _imageX0 % _imageY0
                               % (_imageX0 + mimage.getWidth() - 1) % (_imageY0 + mimage.getHeight() - 1)
                              ).str());
        }
    }
    
    /// @brief method called for each pixel by apply()
    void operator()(typename MaskedImageT::xy_locator iloc, ///< locator pointing at the image pixel
                    int x,                                  ///< column-position of pixel
                    int y                                   ///< row-position of pixel
                   ) {
        double const dx = x - _xcen;
        double const dy = y - _ycen;
        double const du =  dx*_cosTheta + dy*_sinTheta;
        double const dv = -dx*_sinTheta + dy*_cosTheta;

        double r = ::hypot(du, dv*_ab); // ellipsoidal radius
#if 1
        if (::hypot(dx, dy) < 0.5) {    // within a pixel of the centre
            /*
             * We gain significant precision for flattened Gaussians by treating the central pixel specially
             *
             * If the object's centered in the pixel (and has constant surface brightness) we have <r> == eR;
             * if it's at the corner <r> = 2*eR; we interpolate between these exact results linearily in the
             * displacement.  And then add in quadrature which is also a bit dubious
             *
             * We could avoid all these issues by estimating <r> using the same trick as we use for
             * the sinc fluxes; it's not clear that it's worth it.
             */
            
            double const eR = 0.38259771140356325; // <r> for a single square pixel, about the centre
            r = ::hypot(r, eR*(1 + ::hypot(dx, dy)/afw::geom::ROOT2));
        }
#endif

        typename MaskedImageT::Image::Pixel ival = iloc.image(0, 0);
        _sum += ival;
        _sumR += r*ival;
#if 0
        typename MaskedImageT::Variance::Pixel vval = iloc.variance(0, 0);
        _sumVar += vval;
        _sumRVar += r*r*vval;
#endif
    }

    /// Return the Footprint's <r_elliptical>
    double getIr() const { return _sumR/_sum; }

#if 0
    /// Return the variance of the Footprint's <r>
//    double getIrVar() const { return _sumRVar/_sum - getIr()*getIr(); } // Wrong?
    double getIrVar() const { return _sumRVar/(_sum*_sum) + _sumVar*_sumR*_sumR/::pow(_sum, 4); }
#endif

    /// Return whether the measurement might be trusted
    bool getGood() const { return _sum > 0 && _sumR > 0; }

private:
    double const _xcen;                 // center of object
    double const _ycen;                 // center of object
    double const _ab;                   // axis ratio
    double const _cosTheta, _sinTheta;  // {cos,sin}(angle from x-axis)
    double _sum;                        // sum of I
    double _sumR;                       // sum of R*I
#if 0
    double _sumVar;                     // sum of Var(I)
    double _sumRVar;                    // sum of R*R*Var(I)
#endif
    int const _imageX0, _imageY0;       // origin of image we're measuring

};


struct KronAperture {
    KronAperture(afw::geom::Point2D const& center, afw::geom::ellipses::BaseCore const& core) :
        _center(center), _axes(core) {}
    explicit KronAperture(afw::table::SourceRecord const& source) :
        _center(afw::geom::Point2D(source.getX(), source.getY())), _axes(source.getShape()) {}
    KronAperture(afw::table::SourceRecord const& reference, afw::geom::AffineTransform const& refToMeas,
                 double radius) :
        _center(refToMeas(reference.getCentroid())),
        _axes(_getKronAxes(reference.getShape(), refToMeas.getLinear(), radius))
        {}


    /// Accessors
    double getX() const { return _center.getX(); }
    double getY() const { return _center.getY(); }
    afw::geom::Point2D const& getCenter() const { return _center; }
    afw::geom::ellipses::Axes & getAxes() { return _axes; }
    afw::geom::ellipses::Axes const& getAxes() const { return _axes; }

    /// Determine the Kron Aperture from an image
    template<typename ImageT>
    static PTR(KronAperture) determine(ImageT const& image, afw::geom::ellipses::Axes axes,
                                       afw::geom::Point2D const& center,
                                       KronFluxControl const& ctrl, float *radiusForRadius
                                      );

    /// Photometer within the Kron Aperture on an image
    template<typename ImageT>
    std::pair<double, double> measure(ImageT const& image, // Image to measure
                                      double const nRadiusForFlux, // Kron radius multiplier
                                      double const maxSincRadius // largest radius that we use sinc apertyres
                                     ) const;

    /// Transform a Kron Aperture to a different frame
    PTR(KronAperture) transform(afw::geom::AffineTransform const& trans) const {
        afw::geom::Point2D const center = trans(getCenter());
        afw::geom::ellipses::Axes const axes(*getAxes().transform(trans.getLinear()).copy());
        return boost::make_shared<KronAperture>(center, axes);
    }

private:
    /// Determine Kron axes from a reference image
    static
    afw::geom::ellipses::Axes _getKronAxes(
        afw::geom::ellipses::Axes const& shape,
        afw::geom::LinearTransform const& transformation,
        double const radius
        );

    afw::geom::Point2D const _center;     // Center of aperture
    afw::geom::ellipses::Axes _axes;      // Ellipse defining aperture shape
};


afw::geom::ellipses::Axes KronAperture::_getKronAxes(
    afw::geom::ellipses::Axes const& shape,
    afw::geom::LinearTransform const& transformation,
    double const radius
    )
{
    afw::geom::ellipses::Axes axes(shape);
    axes.scale(radius/axes.getDeterminantRadius());
    return axes.transform(transformation);
}


/*
 * Estimate the object Kron aperture, using the shape from source.getShape() (e.g. SDSS's adaptive moments)
 */
template<typename ImageT>
PTR(KronAperture) KronAperture::determine(ImageT const& image, // Image to measure
                                          afw::geom::ellipses::Axes axes,   // Shape of aperture
                                          afw::geom::Point2D const& center, // Centre of source
                                          KronFluxControl const& ctrl,      // control the algorithm
                                          float *radiusForRadius            // radius used to estimate radius
                                         )
{
    //
    // We might smooth the image because this is what SExtractor and Pan-STARRS do.  But I don't see much gain
    //
    double const sigma = ctrl.smoothingSigma; // Gaussian width of smoothing sigma to apply
    bool const smoothImage = sigma > 0;
    int kSize = smoothImage ? 2*int(2*sigma) + 1 : 1;
    afw::math::GaussianFunction1<afw::math::Kernel::Pixel> gaussFunc(smoothImage ? sigma : 100);
    afw::math::SeparableKernel kernel(kSize, kSize, gaussFunc, gaussFunc);
    bool const doNormalize = true, doCopyEdge = false;
    afw::math::ConvolutionControl convCtrl(doNormalize, doCopyEdge);

    double radius0 = axes.getDeterminantRadius();
    double radius = std::numeric_limits<double>::quiet_NaN();
    for (int i = 0; i < ctrl.nIterForRadius; ++i) {
        axes.scale(ctrl.nSigmaForRadius);
        *radiusForRadius = axes.getDeterminantRadius(); // radius we used to estimate R_K
        //
        // Build an elliptical Footprint of the proper size
        //
        afw::detection::Footprint foot(afw::geom::ellipses::Ellipse(axes, center));
        afw::geom::Box2I bbox = !smoothImage ?
            foot.getBBox() :
            kernel.growBBox(foot.getBBox()); // the smallest bbox needed to convolve with Kernel
        bbox.clip(image.getBBox());
        ImageT subImage(image, bbox, afw::image::PARENT, smoothImage);
        if (smoothImage) {
            afw::math::convolve(subImage, ImageT(image, bbox, afw::image::PARENT, false), kernel, convCtrl);
        }
        //
        // Find the desired first moment of the elliptical radius, which corresponds to the major axis.
        //
        FootprintFindMoment<ImageT, afw::detection::Psf::Image> iRFunctor(
                                                    subImage, center, axes.getA()/axes.getB(), axes.getTheta()
                                                                 );

        try {
            iRFunctor.apply(foot);
        } catch(lsst::pex::exceptions::OutOfRangeError &e) {
            if (i == 0) {
                LSST_EXCEPT_ADD(e, "Determining Kron aperture");
            }
            break;                      // use the radius we have
        }
        
        if (!iRFunctor.getGood()) {
            throw LSST_EXCEPT(BadKronException, "Bad integral defining Kron radius");
        }
        
        radius = iRFunctor.getIr()*sqrt(axes.getB()/axes.getA());
        if (radius <= radius0) {
            break;
        }
        radius0 = radius;

        axes.scale(radius/axes.getDeterminantRadius()); // set axes to our current estimate of R_K
    }

    return boost::make_shared<KronAperture>(center, axes);
}

// Photometer an image with a particular aperture
template<typename ImageT>
std::pair<double, double> photometer(
    ImageT const& image, // Image to measure
    afw::geom::ellipses::Ellipse const& aperture, // Aperture in which to measure
    double const maxSincRadius // largest radius that we use sinc apertures to measure
    )
{
    afw::geom::ellipses::Axes const& axes = aperture.getCore();
    if (axes.getB() > maxSincRadius) {
        FootprintFlux<ImageT> fluxFunctor(image);
        afw::detection::Footprint const foot(aperture, image.getBBox());
        fluxFunctor.apply(foot);

        return std::make_pair(fluxFunctor.getSum(), ::sqrt(fluxFunctor.getSumVar()));
    }
    try {
        return algorithms::photometry::calculateSincApertureFlux(image, aperture);
    } catch(pex::exceptions::LengthError &e) {
        LSST_EXCEPT_ADD(e, (boost::format("Measuring Kron flux for object at (%.3f, %.3f);"
                                          " aperture radius %g,%g theta %g")
                            % aperture.getCenter().getX() % aperture.getCenter().getY()
                            % axes.getA() % axes.getB() % afw::geom::radToDeg(axes.getTheta())).str());
        throw e;
    }
}


double calculatePsfKronRadius(
    CONST_PTR(afw::detection::Psf) const& psf, // PSF to measure
    afw::geom::Point2D const& center, // Centroid of source on parent image
    double smoothingSigma=0.0         // Gaussian sigma of smoothing applied
    )
{
    assert(psf);
    double const radius = psf->computeShape(center).getDeterminantRadius();
    // For a Gaussian N(0, sigma^2), the Kron radius is sqrt(pi/2)*sigma
    return ::sqrt(afw::geom::PI/2)*::hypot(radius, std::max(0.0, smoothingSigma));
}

template<typename ImageT>
std::pair<double, double> KronAperture::measure(ImageT const& image, // Image of interest
                                                double const nRadiusForFlux, // Kron radius multiplier
                                                double const maxSincRadius // largest radius that we use sinc
                                                                           // apertures to measure
                                               ) const
{
    afw::geom::ellipses::Axes axes(getAxes()); // Copy of ellipse core, so we can scale
    axes.scale(nRadiusForFlux);
    afw::geom::ellipses::Ellipse const ellip(axes, getCenter());

    return photometer(image, ellip, maxSincRadius);
}

/************************************************************************************************************/
/*
 * Apply the algorithm to the PSF model
 */
double
getPsfFactor(CONST_PTR(afw::detection::Psf) psf,
             afw::geom::Point2D const& center,
             double const R_K,
             double const maxSincRadius
            )
{
    typedef afw::detection::Psf::Image PsfImageT;
    PTR(PsfImageT) psfImage; // the image of the PSF

    if (!psf) {
        return 1.0;
    }

    int const pad = 5;
    try {
        PTR(PsfImageT) psfImageNoPad = psf->computeImage(center); // Unpadded image of PSF
        
        psfImage = PTR(PsfImageT)(
            new PsfImageT(psfImageNoPad->getDimensions() + afw::geom::Extent2I(2*pad))
            );
        afw::geom::BoxI middleBBox(afw::geom::Point2I(pad, pad), psfImageNoPad->getDimensions());
        
        PTR(PsfImageT) middle(new PsfImageT(*psfImage, middleBBox, afw::image::LOCAL));
        *middle <<= *psfImageNoPad;
    } catch (lsst::pex::exceptions::Exception & e) {
        LSST_EXCEPT_ADD(e, (boost::format("Computing PSF at (%.3f, %.3f)")
                            % center.getX() % center.getY()).str());
        throw e;
    }
    // Measure the Kron flux for the Psf
    int const psfXCen = 0.5*(psfImage->getWidth() - 1); // Center of (21x21) image is (10.0, 10.0)
    int const psfYCen = 0.5*(psfImage->getHeight() - 1);
    // Grrr. calculateSincApertureFlux can't handle an Image
    PTR(afw::image::MaskedImage<PsfImageT::Pixel>) mi(new afw::image::MaskedImage<PsfImageT::Pixel>(psfImage));
    afw::geom::ellipses::Ellipse aperture(afw::geom::ellipses::Axes(R_K, R_K),
                                          afw::geom::Point2D(psfXCen, psfYCen));
    return photometer(*mi, aperture, maxSincRadius).first;
}

/************************************************************************************************************/

template <typename PixelT>
void KronFlux::_applyAperture(
    afw::table::SourceRecord & source,
    afw::image::Exposure<PixelT> const& exposure,
    KronAperture const& aperture
    ) const
{
    KronFluxControl const& ctrl = static_cast<KronFluxControl const&>(this->getControl());

    double const rad = aperture.getAxes().getDeterminantRadius();
    if (rad < std::numeric_limits<double>::epsilon()) {
        source.set(_badRadiusKey, true);
        throw LSST_EXCEPT(lsst::pex::exceptions::UnderflowError,
                          str(boost::format("Kron radius is < epsilon for source %ld") % source.getId()));
    }

    std::pair<double, double> result;
    try {
        result = aperture.measure(exposure.getMaskedImage(), ctrl.nRadiusForFlux, ctrl.maxSincRadius);
    } catch (pex::exceptions::LengthError const& e) {
        // We hit the edge of the image; there's no reasonable fallback or recovery
        source.set(getKeys().flag, true);
        source.set(_edgeKey, true);
        throw;
    }

    source.set(getKeys().meas, result.first);
    source.set(getKeys().err, result.second);
    source.set(_radiusKey, aperture.getAxes().getDeterminantRadius());
}

template <typename PixelT>
void KronFlux::_apply(
                      afw::table::SourceRecord & source,
                      afw::image::Exposure<PixelT> const& exposure,
                      afw::geom::Point2D const & center
                     ) const {
    source.set(getKeys().flag, true); // bad unless we get all the way to success at the end
    source.set(_badRadiusKey, false);  // innocent until proven guilty
    source.set(_smallRadiusKey, false); // innocent until proven guilty
    source.set(_badShapeKey, false); // innocent until proven guilty
    source.set(_usedMinimumRadiusKey, false);
    source.set(_usedPsfRadiusKey, false);

    // Did we hit a condition that fundamentally prevented measuring the Kron flux?
    // Such conditions include hitting the edge of the image and bad input shape, but not low signal-to-noise.
    bool bad = false;

    afw::image::MaskedImage<PixelT> const& mimage = exposure.getMaskedImage();

    KronFluxControl const & ctrl = static_cast<KronFluxControl const &>(this->getControl());

    double R_K_psf = -1;
    if (exposure.getPsf()) {
        R_K_psf = calculatePsfKronRadius(exposure.getPsf(), center, ctrl.smoothingSigma);
    }

    //
    // Get the shape of the desired aperture
    //
    afw::geom::ellipses::Axes axes;
    if (!source.getShapeFlag()) {
        axes = source.getShape();
    } else {
        bad = true;
        if (!exposure.getPsf()) {
            throw LSST_EXCEPT(pex::exceptions::RuntimeError, "Bad shape and no PSF");
        }
        axes = exposure.getPsf()->computeShape();
        source.set(_badShapeKey, true);
    }
    if (ctrl.useFootprintRadius) {
        afw::geom::ellipses::Axes footprintAxes(source.getFootprint()->getShape());
        // if the Footprint's a disk of radius R we want footRadius == R.
        // As <r^2> = R^2/2 for a disk, we need to scale up by sqrt(2)
        footprintAxes.scale(::sqrt(2));

        double radius0 = axes.getDeterminantRadius();
        double const footRadius = footprintAxes.getDeterminantRadius();

        if (footRadius > radius0*ctrl.nSigmaForRadius) {
            radius0 = footRadius/ctrl.nSigmaForRadius; // we'll scale it up by nSigmaForRadius
            axes.scale(radius0/axes.getDeterminantRadius());
        }
    }

    PTR(KronAperture) aperture;
    float radiusForRadius = std::numeric_limits<double>::quiet_NaN();
    if (ctrl.fixed) {
        aperture.reset(new KronAperture(source));
    } else {
        try {
            aperture = KronAperture::determine(mimage, axes, center, ctrl, &radiusForRadius);
        } catch (pex::exceptions::OutOfRangeError& e) {
            // We hit the edge of the image: no reasonable fallback or recovery possible
            source.set(_edgeKey, true);
            source.set(getKeys().flag, true);
            throw;
        } catch (BadKronException& e) {
            // Not setting bad=true because we only failed due to low S/N
            aperture = _fallbackRadius(source, R_K_psf, e);
        } catch(pex::exceptions::Exception& e) {
            bad = true; // There's something fundamental keeping us from measuring the Kron aperture
            aperture = _fallbackRadius(source, R_K_psf, e);
        }
    }

    /*
     * Estimate the minimum acceptable Kron radius as the Kron radius of the PSF or the provided minimum radius
     */

    // Enforce constraints on minimum radius
    double rad = aperture->getAxes().getDeterminantRadius();
    if (ctrl.enforceMinimumRadius) {
        double newRadius = rad;
        if (ctrl.minimumRadius > 0.0) {
            if (rad < ctrl.minimumRadius) {
                newRadius = ctrl.minimumRadius;
                source.set(_usedMinimumRadiusKey, true);
            }
        } else if (!exposure.getPsf()) {
            throw LSST_EXCEPT(lsst::pex::exceptions::RuntimeError,
                              "No minimum radius and no PSF provided");
        } else if (rad < R_K_psf) {
            newRadius = R_K_psf;
            source.set(_usedPsfRadiusKey, true);
        }
        if (newRadius != rad) {
            aperture->getAxes().scale(newRadius/rad);
            source.set(_smallRadiusKey, true); // guilty after all
        }
    }

    _applyAperture(source, exposure, *aperture);
    source.set(_radiusForRadiusKey, radiusForRadius);
    source.set(_psfRadiusKey, R_K_psf);
    source.set(getKeys().flag, bad);
}

template <typename PixelT>
void KronFlux::_applyForced(
        afw::table::SourceRecord & source,
        afw::image::Exposure<PixelT> const & exposure,
        afw::geom::Point2D const & center,
        afw::table::SourceRecord const & reference,
        afw::geom::AffineTransform const & refToMeas
    ) const
{
    source.set(getKeys().flag, true); // bad unless we get all the way to success at the end
    KronFluxControl const& ctrl = static_cast<KronFluxControl const &>(this->getControl());
    float const radius = reference.get(reference.getSchema().find<float>(ctrl.name + ".radius").key);
    KronAperture const aperture(reference, refToMeas, radius);
    _applyAperture(source, exposure, aperture);
    if (exposure.getPsf()) {
        source.set(_psfRadiusKey, calculatePsfKronRadius(exposure.getPsf(), center, ctrl.smoothingSigma));
    }
    source.set(getKeys().flag, false);
}


PTR(KronAperture) KronFlux::_fallbackRadius(afw::table::SourceRecord& source, double const R_K_psf,
                                            pex::exceptions::Exception& exc) const
{
    KronFluxControl const & ctrl = static_cast<KronFluxControl const &>(this->getControl());
    source.set(_badRadiusKey, true);
    double newRadius;
    if (ctrl.minimumRadius > 0) {
        newRadius = ctrl.minimumRadius;
        source.set(_usedMinimumRadiusKey, true);
    } else if (R_K_psf > 0) {
        newRadius = R_K_psf;
        source.set(_usedPsfRadiusKey, true);
    } else {
        LSST_EXCEPT_ADD(exc, "Bad Kron aperture, no minimum radius specified, and no PSF");
        throw exc;
    }
    PTR(KronAperture) aperture(new KronAperture(source));
    aperture->getAxes().scale(newRadius/aperture->getAxes().getDeterminantRadius());
    return aperture;
}



LSST_MEAS_ALGORITHM_PRIVATE_IMPLEMENTATION(KronFlux);

} // anonymous namespace

PTR(algorithms::AlgorithmControl) KronFluxControl::_clone() const {
    return boost::make_shared<KronFluxControl>(*this);
}

PTR(algorithms::Algorithm) KronFluxControl::_makeAlgorithm(
    afw::table::Schema & schema,
    PTR(daf::base::PropertyList) const &
) const {
    return boost::make_shared<KronFlux>(*this, boost::ref(schema));
}


}}}} // namespace lsst::meas::extensions::photometryKron
