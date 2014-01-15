#include "Halide.h"

#include "adaptbp.h"

#include "cvutil/cvutil.h"

#include <tuple>
#include <vector>
#include <map>

using namespace std;

// TODO Wrap all of this in a class

void computeDisparity(
        const CImg<int16_t>& leftImg,
        const CImg<int16_t>& rightImg,
        int minDisp,
        int maxDisp,
        float omega,
        CImg<int16_t>& disparityImg) {
    assert(leftImg.is_sameXYZC(rightImg));
    
    Halide::Buffer leftBuf(
            Halide::Int(16),
            leftImg.width(), leftImg.height(),
            1, leftImg.spectrum(),
            (uint8_t*) leftImg.data(), string("leftBuf"));

    Halide::Buffer rightBuf(
            Halide::Int(16),
            rightImg.width(), rightImg.height(),
            1, rightImg.spectrum(),
            (uint8_t*) rightImg.data(), string("rightBuf"));

    // Inputs
    Halide::Image<int16_t> left(leftBuf.raw_buffer(), string("left"));
    Halide::Image<int16_t> right(rightBuf.raw_buffer(), string("right"));

    Halide::Param<float> omegaParam;
    Halide::Param<int> minDispParam;
    Halide::Param<int> maxDispParam;

    // TODO omega should be dynamically chosen
    omegaParam.set(omega);
    minDispParam.set(minDisp);
    maxDispParam.set(maxDisp);

    // Variables
    
    Halide::Var x("x"), y("y"), c("c"), d("d");
    
    Halide::RDom rDisp(minDispParam, maxDispParam);
    Halide::RDom rC(0, 2);

    int wndRad = 1;
    Halide::RDom r3x3(-wndRad, wndRad, -wndRad, wndRad);
    Halide::RDom r3x2(-wndRad, 0, -wndRad, wndRad);
    Halide::RDom r2x3(-wndRad, wndRad, -wndRad, 0);

    // Helper expressions to clamp to image bounds
    
    Halide::Expr cx = clamp(x,
            max(-minDispParam, 1),
            min(left.width() - maxDispParam, left.width() - 2));

    Halide::Expr cy = clamp(y, 1, left.height() - 2);

    Halide::Func leftC("leftC");
    Halide::Func rightC("rightC");
    leftC(x, y, c) = left(cx, cy, 0, c);
    rightC(x, y, c) = right(cx, cy, 0, c);
    
    // C_SAD(x, y, c, d) ...
    Halide::Func absDiff("absDiff"), cSAD("cSAD");

    absDiff(x, y, c, d) = Halide::abs(leftC(x, y, c) - rightC(x + d, y, c));

    cSAD(x, y, c, d) += absDiff(x + r3x3.x, y + r3x3.y, c, d);

    // C_GRAD(x, y, c, d)...
    Halide::Func gradX1("gradX1"), gradX2("gradX2");
    Halide::Func gradY1("gradY1"), gradY2("gradY2");
    Halide::Func absGradX("absGradX"), absGradY("absGradY");
    Halide::Func cGrad("cGrad");
    
    gradX1(x, y, c) = leftC(x + 1, y, c) - leftC(x, y, c);
    gradX2(x, y, c) = rightC(x + 1, y, c) - rightC(x, y, c);

    gradY1(x, y, c) = leftC(x, y + 1, c) - leftC(x, y, c);
    gradY2(x, y, c) = rightC(x, y + 1, c) - rightC(x, y, c);

    absGradX(x, y, c, d) = Halide::abs(gradX1(x, y, c) - gradX2(x + d, y, c));

    absGradY(x, y, c, d) = Halide::abs(gradY1(x, y, c) - gradY2(x + d, y, c));

    cGrad(x, y, c, d) +=
        absGradX(x + r3x2.x, y + r3x2.y, c, d) +
        absGradY(x + r3x2.x, y + r3x2.y, c, d);

    // C(x, y, d)...
    Halide::Func cost("cost");
    // TODO Robustify cSAD and cGrad with a max distance learned from optimization
    cost(x, y, d) += 
        (1.0f - omegaParam) * Halide::cast(Halide::Float(32), cSAD(x, y, rC, d)) +
        omegaParam * Halide::cast(Halide::Float(32), cGrad(x, y, rC, d));

    // Argmin_d(x, y)...
    Halide::Func minCostDisparity("minCostDisparity");

    minCostDisparity(x, y) = Halide::cast(Halide::Int(16), 0);

    Halide::Expr bestDisparitySoFar =
        cost(x, y, clamp(minCostDisparity(x, y), minDispParam, maxDispParam));

    minCostDisparity(x, y) = select(
            cost(x, y, rDisp) < bestDisparitySoFar,
            Halide::cast(Halide::Int(16), rDisp),
            minCostDisparity(x, y));
    
    // Argmin_d_reverse(x, y)...
    Halide::Func costRev("costRev");

    costRev(x, y, d) = cost(x + d, y, -d);

    Halide::Func minCostDisparityRev("minCostDisparityRev");

    minCostDisparityRev(x, y) = Halide::cast(Halide::Int(16), 0);

    Halide::Expr bestDisparitySoFarRev =
        costRev(x, y, clamp(minCostDisparityRev(x, y), -maxDispParam, -minDispParam));

    minCostDisparityRev(x, y) = select(
            costRev(x, y, -rDisp) < bestDisparitySoFarRev,
            Halide::cast(Halide::Int(16), -rDisp),
            minCostDisparityRev(x, y));

    // Holes(x, y)...
    Halide::Expr revX = minCostDisparity(x, y) + x;
    Halide::Expr revXC = Halide::clamp(revX, 0, left.width() - 1);

    Halide::Expr consistent = Halide::select(revXC == revX,
            Halide::abs(
                minCostDisparityRev(revXC, y) + minCostDisparity(x, y)
                ) < 2.0f,
            false);

    // Result(x, y)...
    Halide::Func result("result");

    // TODO Only compute over valid range of x values (ignore left & right border)
    Halide::Expr validX = Halide::clamp(
            x,
            Halide::max(0, -minDispParam),
            Halide::min(left.width() - 1, left.width() - maxDispParam));
    
    result(x, y) =
        Halide::select(consistent,
                Halide::cast(Halide::Int(16),
                    Halide::select(x == validX,
                        minCostDisparity(validX, y),
                        Halide::Int(16).max()
                        )
                    ),
                Halide::Int(16).max());

    // Schedule...
    absDiff
        .reorder(x, c, d, y)
        .compute_at(cSAD, x)
        .store_at(cSAD, d)
        .bound(c, 0, 2);

    cSAD
        .reorder(x, c, d, y)
        .compute_at(cost, x)
        .store_root()
        .bound(c, 0, 2);

    gradX1
        .reorder(x, c, y)
        .compute_at(absGradX, x)
        .store_root()
        .bound(c, 0, 2);

    gradX2
        .reorder(x, c, y)
        .compute_at(absGradX, x)
        .store_root()
        .bound(c, 0, 2);

    gradY1
        .reorder(x, c, y)
        .compute_at(absGradY, x)
        .store_root()
        .bound(c, 0, 2);

    gradY2
        .reorder(x, c, y)
        .compute_at(absGradY, x)
        .store_root()
        .bound(c, 0, 2);

    absGradX
        .reorder(x, c, y)
        .compute_at(cGrad, x)
        .store_root()
        .bound(c, 0, 2);

    absGradY
        .reorder(x, c, y)
        .compute_at(cGrad, x)
        .store_root()
        .bound(c, 0, 2);

    cGrad
        .reorder(x, c, d, y)
        .compute_at(cost, x)
        .store_root();

    cost
        .reorder(x, y)
        .compute_at(result, y)
        .store_root();

    minCostDisparity
        .reorder(x, y)
        .compute_at(result, y)
        .store_root()
        .bound(x, 0, left.width())
        .bound(y, 0, left.height());

    minCostDisparityRev
        .reorder(x, y)
        .compute_at(result, y)
        .store_root()
        .bound(x, 0, left.width())
        .bound(y, 0, left.height());

    result.compute_inline()
        .reorder(x, y);

    vector<Halide::Argument> args;
    args.push_back(left);
    args.push_back(right);
    args.push_back(minDispParam);
    args.push_back(maxDispParam);
    args.push_back(omegaParam);

    printf("Compiling...\n");
    // result.compile_to_c("result.cpp", args);
    result.compile_jit();
    printf("Done\n");

    printf("Running...\n");
    Halide::Realization r = result.realize(leftImg.width(), leftImg.height());
    printf("Done...\n");

    // Copy out the results from buffer to CImg
    {
        Halide::Buffer disparityBuf = r[0];
        Halide::Image<int16_t> disparityI(disparityBuf);

        disparityImg= CImg<int16_t>(leftImg.width(), leftImg.height());
        cimg_forXY(disparityImg, x, y) {
            disparityImg(x, y) = disparityI(x, y);
        }

    }
}

/**
 * Estimates slant (dD/dt) from a set of samples of (D, t)
 * samples for which all non-t dimensions are constant.
 *
 * For example, if dSamples contains a set of (D, x) samples
 * from the same horizontal scan-line (fixed y-coordinate),
 * it will estimate dD/dx.
 *
 * Returns true upon success, false if not enough samples were provided.
 */
inline bool estimateSlant(
        const map<uint16_t, vector<tuple<uint16_t, int16_t>>>& dSamples,
        float& result) {
    int totalSamplePairs = 0;
    for (const auto& samples : dSamples) {
        int n = samples.second.size() - 1;
        totalSamplePairs += n * (n + 1) / 2;
    }

    if (totalSamplePairs < 1) {
        return false;
    }

    // Store all possible samples of dt, each consisting of a finite difference
    // between elements in the same scanline, as given by dSamples.
    CImg<float> dtSamples(totalSamplePairs);

    // Index into dtSamples at which to insert new samples
    int dtSamplesI = 0;

    for (const auto& samplesPair: dSamples) {
        const vector<tuple<uint16_t, int16_t>>& samples = samplesPair.second;

        for (int i = 0; i < samples.size(); i++) {
            for (int j = i + 1; j < samples.size(); j++) {
                assert(dtSamplesI < dtSamples.width());
                dtSamples(dtSamplesI) = 
                    ((float) get<1>(samples[j]) - get<1>(samples[i])) /
                    ((float) get<0>(samples[j]) - get<0>(samples[i]));
                dtSamplesI++;
            }
        }
    }

    dtSamples.sort();

    // TODO The paper doesn't specify blur kernel size!
    // dtSamples.blur(dtSamples.size() / 3.0f);

    result = dtSamples(dtSamples.size() / 2);

    return true;
}


void fitPlanes(
        const vector<vector<tuple<uint16_t, uint16_t>>>& superpixels,
        const CImg<int16_t>& disp,
        vector<Plane>& planes) {
    // Create a plane for each superpixel
    planes = vector<Plane>(superpixels.size());

    // A map from y-index to (x, disparity) tuples to store
    // valid disparities for each scan-line in a superpixel.
    map<uint16_t, vector<tuple<uint16_t, int16_t>>> xDSamples;
    
    // A map from x-index to (y, disparity) tuples to store
    // valid disparities for each vertical-line in a superpixel.
    map<uint16_t, vector<tuple<uint16_t, int16_t>>> yDSamples;

    for (int superpixelI = 0; superpixelI < superpixels.size(); superpixelI++) {
        const auto& pixels = superpixels[superpixelI];

        xDSamples.clear();
        yDSamples.clear();

        int numValidD = 0;

        // Iterate over all pixels within the superpixel
        for (const auto& p : pixels) {
            uint16_t x = get<0>(p);
            uint16_t y = get<1>(p);

            // If this pixel has a valid disparity, add it
            if (disp(x, y) != std::numeric_limits<int16_t>::max()) {
                xDSamples[y].push_back(make_tuple(x, disp(x, y)));
                yDSamples[x].push_back(make_tuple(y, disp(x, y)));

                numValidD++;
            }
        }
        
        float cx, cy;

        if (!estimateSlant(xDSamples, cx)) {
            continue;
        }

        if (!estimateSlant(yDSamples, cy)) {
            continue;
        }

        CImg<float> cSamples(numValidD);
        int cSamplesI = 0;

        // Iterate again, collecting samples with which to estimate
        // the 'c' value for the plane
        for (const auto& p : pixels) {
            uint16_t x = get<0>(p);
            uint16_t y = get<1>(p);

            if (disp(x, y) != std::numeric_limits<int16_t>::max()) {
                float c = disp(x, y) - (cx * x + cy * y);

                cSamples(cSamplesI) = c;
                cSamplesI++;
            }
        }

        // TODO Paper doesn't specify how much to blur
        // cSamples.blur(cSamples.width() / 3.0f);

        float c = cSamples(cSamples.width() / 2);

        planes[superpixelI] = Plane(cx, cy, c);
    }
}

void superpixelPlanesToDisparity(
        const vector<vector<tuple<uint16_t, uint16_t>>>& superpixels,
        const vector<Plane>& planes,
        CImg<float>& disp) {
    for (int superpixelI = 0; superpixelI < superpixels.size(); superpixelI++) {
        const auto& pixels = superpixels[superpixelI];

        if (planes[superpixelI].isValid()) {
            // Iterate over all pixels within the superpixel
            for (const auto& p : pixels) {
                uint16_t x = get<0>(p);
                uint16_t y = get<1>(p);

                disp(x, y) = planes[superpixelI].dispAt(x, y);
            }
        }
    }
}

void superpixelPlaneCost(
        const CImg<float>& left,
        const CImg<float>& right,
        const vector<vector<tuple<uint16_t, uint16_t>>>& superpixels,
        float omega,
        const vector<Plane>& planes,
        CImg<float>& segmentPlaneCost) {
    // FIXME Don't assume that the number of planes and the number of segments are equal!
    
    int N = superpixels.size();

    segmentPlaneCost = CImg<float>(N, N);

    segmentPlaneCost = 0.0f;

    // '1' specifies forward finite differences
    CImgList<float> leftGrad = left.get_gradient(0, 1);
    CImgList<float> rightGrad = right.get_gradient(0, 1);

    vector<tuple<int, Plane>> validPlanes;

    for (int planeI = 0; planeI < N; planeI++) {
        const Plane& plane = planes[planeI];
        if (plane.isValid()) {
            validPlanes.push_back(make_tuple(planeI, plane));
        } else {
            for (int segmentI = 0; segmentI < N; segmentI++) {
                segmentPlaneCost(segmentI, planeI) = std::numeric_limits<float>::max();
            }
        }
    }

    // TODO Optimize - Store bounding-box for superpixel, create early-out if
    //                 a plane transforms the bounding-box outside of the image.
    for (int superpixelI = 0; superpixelI < superpixels.size(); superpixelI++) {
        const auto& pixels = superpixels[superpixelI];
        printf("Processing superpixel %d\n", superpixelI);

        for (const auto& indexedPlane : validPlanes) {
            int planeI = get<0>(indexedPlane);
            const Plane& plane = get<1>(indexedPlane);

            // Iterate over all pixels within the superpixel
            for (const auto& p : pixels) {
                uint16_t x = get<0>(p);
                uint16_t y = get<1>(p);

                int rx = (int) (x + plane.dispAt(x, y) + 0.5f);
                int ry = y;

                if (rx < 0 || rx > right.width() - 2) {
                    segmentPlaneCost(superpixelI, planeI) =
                        std::numeric_limits<float>::max();
                    break;
                }

                float cost = 0;

                cimg_forZC(left, z, c) {
                    float sad = abs(right(rx, ry, z, c) -
                            left(x, y, z, c));

                    float grad = 0;

                    grad += abs(leftGrad(0)(x, y, z, c) -
                            rightGrad(0)(rx, ry, z, c));

                    grad += abs(leftGrad(1)(x, y, z, c) -
                            rightGrad(1)(rx, ry, z, c));

                    // FIXME Robustify this by truncating against value
                    //       determined by the mean & sd of these for reliable
                    //       disparities found in the first step.

                    cost += (1.0f - omega) * sad + omega * grad;
                }

                segmentPlaneCost(superpixelI, planeI) += cost;
            }
        }
    }
}

void refinePlanes(
        const CImg<float>& left,
        const CImg<float>& right,
        const CImg<float>& disp,
        const vector<vector<tuple<uint16_t, uint16_t>>>& superpixels,
        const vector<Plane>& planes,
        float omega,
        vector<Plane>& refinedPlanes) {
    // The number of planes and segments. Note that it is assumed that
    // there is exactly one plane for each segment.

    // FIXME work with different number of planes and segments
    int N = planes.size();

    refinedPlanes = vector<Plane>(N);

    CImg<float> segmentPlaneCost;
    superpixelPlaneCost(left, right, superpixels, omega,
            planes, segmentPlaneCost);

    segmentPlaneCost.display();

    // Map from each plane to the set of segments for which it is optimal
    map<int, vector<int>> planeSegments;

    for (int segmentI = 0; segmentI < N; segmentI++) {
        int optimalPlaneI = 0;
        float optimalPlaneCost = std::numeric_limits<float>::max();

        for (int planeI = 0; planeI < N; planeI++) {
            float cost = segmentPlaneCost(segmentI, planeI);
            
            if (cost < optimalPlaneCost) {
                optimalPlaneCost = cost;
                optimalPlaneI = planeI;
            }
        }

        planeSegments[optimalPlaneI].push_back(segmentI);
    }
    
    // Create new superpixel vector by merging superpixels with the same optimal plane
    vector<vector<tuple<uint16_t, uint16_t>>> mergedSuperpixels(planeSegments.size());

    int mergedSegmentI = 0;
    for (const auto& ps : planeSegments) {
        const vector<int>& segments = ps.second;

        for (int segmentI : segments) {
            auto& segment = mergedSuperpixels[mergedSegmentI];

            segment.insert(segment.end(), superpixels[segmentI].begin(),
                    superpixels[segmentI].end());
        }

        mergedSegmentI++;
    }

    fitPlanes(mergedSuperpixels, disp, refinedPlanes);

    CImg<float> pDisp(disp.width(), disp.height());
    pDisp = 0.0f;
    superpixelPlanesToDisparity(mergedSuperpixels, refinedPlanes, pDisp);
    pDisp.display();
}

/**
 * Computes stereo correspondence based on 
 *
 * Segment-Based Stereo Matching Using Belief Propagation and a Self-Adapting
 * Dissimilarity Measure (by Klause, Sormann, and Karner)
 *
 * a.k.a "AdaptBP" in Middlebury rankings
 *
 */
void computeAdaptBPStereo(
        const CImg<int16_t>& left,
        const CImg<int16_t>& right,
        int minDisp,
        int maxDisp,
        CImg<int16_t>& disp) {
    assert(left.is_sameXYZC(right));

    /**
     * Compute a segmentation via Slic superpixelization.
     *
     * Note that this differs from the original paper, which used
     * Mean-shift color segmentation (Comaniciu and Meer)
     */
    CImg<int> segmentation;
    vector<vector<tuple<uint16_t, uint16_t>>> superpixels;

    int numSuperpixels = 512;

    printf("Number of superpixels: %d\n", numSuperpixels);

    printf("Computing superpixels for Left\n");
    slicSuperpixels(left.get_RGBtoLab(), numSuperpixels, 10, segmentation, superpixels);

    segmentation.display();

    printf("Computing disparity\n");
    computeDisparity(left.get_RGBtoLab(), right.get_RGBtoLab(),
            minDisp, maxDisp, 0.5f, disp);

    disp.display();

    vector<Plane> planes;
    printf("Fiting planes...\n");
    fitPlanes(superpixels, disp, planes);

    CImg<float> pDisp(disp.width(), disp.height());
    pDisp = 0.0f;
    superpixelPlanesToDisparity(superpixels, planes, pDisp);
    pDisp.display();

    vector<Plane> refinedPlanes;
    refinePlanes(left, right, disp, superpixels, planes, 0.5f, refinedPlanes);
}
