#include "tri_qpbo.h"

#include <random>

#include "ceres/ceres.h"
#include "glog/logging.h"

#include "opengm/graphicalmodel/graphicalmodel.hxx"
#include "opengm/graphicalmodel/space/simplediscretespace.hxx"
#include "opengm/functions/explicit_function.hxx"
#include "opengm/functions/sparsemarray.hxx"
#include "opengm/operations/adder.hxx"
#include "opengm/operations/minimizer.hxx"
#include "opengm/inference/mqpbo.hxx"
#include "opengm/inference/graphcut.hxx"
#include "opengm/inference/auxiliary/minstcutboost.hxx"

#include "boost/polygon/voronoi.hpp"

using boost::polygon::voronoi_builder;
using boost::polygon::voronoi_diagram;

// Create required concepts and traits to use Eigen::Vector2f with boost's
// polygon library.
namespace boost {
    namespace polygon {
        template<>
            struct geometry_concept<Eigen::Vector2f> {
                typedef point_concept type;
            };

        template<>
            struct point_traits<Eigen::Vector2f> {
                typedef int coordinate_type;

                static inline coordinate_type get(
                        const Eigen::Vector2f& point,
                        boost::polygon::orientation_2d orient) {
                    if (orient == boost::polygon::HORIZONTAL) {
                        return (int) point.x();
                    } else {
                        return (int) point.y();
                    }
                }
            };
    }
}

struct TriQPBO::GModelData {
    typedef opengm::SimpleDiscreteSpace<size_t, size_t> Space;
    typedef opengm::ExplicitFunction<float, size_t, size_t> ExplicitFunction;

    typedef opengm::GraphicalModel<float, opengm::Adder, ExplicitFunction,
            Space> GModel;

    typedef opengm::external::QPBO<GModel> QPBO;

    vector<GModel::FunctionIdentifier> binaryFIds;
    vector<GModel::FunctionIdentifier> unaryFIds;

    GModel model;

    unique_ptr<QPBO> qpbo;
};

TriQPBO::TriQPBO(
        const CImg<uint8_t>& lab,
        const vector<Eigen::Vector2f>& _points,
        const vector<double>& _initValue) :
    imgLab(lab),
    points(_points),
    existingValue(_initValue),
    edgeCount(0),
    gModelData(nullptr) {

    newValue.resize(existingValue.size());
    initDelaunay();

    initGModel();
}

TriQPBO::~TriQPBO() {
    delete gModelData;
}

void TriQPBO::denseInterp(
        CImg<double>& result) {
    for (int i = 0; i < triangles.size(); i++) {
        const auto& tri = triangles[i];

        // The following based on the draw_triangle method from CImg.h
        // Note that the gouroud shading and color interpolation implementations
        // in CImg only work on 8-bit integers.
        const double x0 = points[get<0>(tri)].x();
        const double y0 = points[get<0>(tri)].y();
        const double x1 = points[get<1>(tri)].x();
        const double y1 = points[get<1>(tri)].y();
        const double x2 = points[get<2>(tri)].x();
        const double y2 = points[get<2>(tri)].y();
        const double& v0 = existingValue[get<0>(tri)];
        const double& v1 = existingValue[get<1>(tri)];
        const double& v2 = existingValue[get<2>(tri)];

        int nx0 = x0, ny0 = y0, nx1 = x1, ny1 = y1, nx2 = x2, ny2 = y2;
        if (ny0>ny1) cimg::swap(nx0,nx1,ny0,ny1);
        if (ny0>ny2) cimg::swap(nx0,nx2,ny0,ny2);
        if (ny1>ny2) cimg::swap(nx1,nx2,ny1,ny2);
        if (ny0<result.height() && ny2>=0) {
            _cimg_for_triangle1(result,xl,xr,y,nx0,ny0,nx1,ny1,nx2,ny2) {
                int xleft = xl;
                int xright = xr;

                if (xright < xleft) {
                    swap(xleft, xright);
                }

                for (int x = max(xleft, 0);
                        x <= min(xright, result.width() - 1);
                        x++) {
                    double l0 =
                        ((y1 - y2) * (x - x2) + (x2 - x1) * (y - y2)) /
                        ((y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2));
                    double l1 =
                        ((y2 - y0) * (x - x2) + (x0 - x2) * (y - x2)) / 
                        ((y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2));
                    double l2 = 1.0 - l1 - l2;
                    double v = v0 * l0 + v1 * l1 + v2 * l2;
                    result(x, y) = v0;
                }
            }
        }
    }
}

void TriQPBO::visualizeTriangulation(
        CImg<uint8_t>& colorVis) {
    uint8_t color[3];

    const CImg<uint8_t>& lines = CImg<uint8_t>::lines_LUT256();

    for (int i = 0; i < triangles.size(); i++) {
        const auto& tri = triangles[i];

        cimg_forC(lines, c) {
            double cx = 
                points[get<0>(tri)].x() +
                points[get<1>(tri)].x() +
                points[get<2>(tri)].x();
            cx /= 3.0;
            double cy =
                points[get<0>(tri)].y() +
                points[get<1>(tri)].y() +
                points[get<2>(tri)].y();
            cy /= 3.0;

            color[c] = imgLab.linear_atXY(cx, cy, 0, c);
        }

        colorVis.draw_triangle(
                points[get<0>(tri)].x(),
                points[get<0>(tri)].y(),
                points[get<1>(tri)].x(),
                points[get<1>(tri)].y(),
                points[get<2>(tri)].x(),
                points[get<2>(tri)].y(),
                color);
    }
}

struct LineError {
    LineError(
            double _x,
            double _y) :
        x(_x),
        y(_y) {
    }

    template <typename T>
        bool operator()(
                const T* const m,
                const T* const b,
                T* residuals) const {
            residuals[0] = m[0] * T(x) + b[0] - T(y);
        }

    double x, y;
};

void TriQPBO::fitCandidateValuesLinear() {
    // 1 residuals
    // 1 parameters in block 1 (slope)
    // 1 parameters in block 2 (offset)
    typedef ceres::AutoDiffCostFunction<LineError, 1, 1, 1>
        LineCostFunction;

    vector<unique_ptr<LineCostFunction>> costFunctions;

    vector<double> mEstimates;

    for (size_t i = 0; i < points.size(); i++) {
        if (newValue[i] > 0 && existingValue[i] > 0) {

            LineCostFunction* costFunction =
                new LineCostFunction(new LineError(newValue[i], existingValue[i]));

            costFunctions.push_back(unique_ptr<LineCostFunction>(costFunction));

            mEstimates.push_back(existingValue[i] / newValue[i]);
        }
    }

    if (costFunctions.size() == 0) {
        return;
    }

    // Perform RANSAC by randomly selecting cost function terms, solving for
    // model parameters, and counting inliers from the resulting fit.

    // Keep track of the currently-optimal score and value
    double optimalCost = numeric_limits<double>::max();
    double optimalM, optimalB;

    ceres::Problem::Options pOptions;
    // This ensures that ceres don't delete our cost functions
    pOptions.cost_function_ownership = ceres::Ownership::DO_NOT_TAKE_OWNERSHIP;
    pOptions.loss_function_ownership = ceres::Ownership::DO_NOT_TAKE_OWNERSHIP;

    std::random_shuffle(mEstimates.begin(), mEstimates.end());

    // Perform several iterations of RANSAC
    // TODO compute this based on acceptable probability of success
    const int max_iters = min(200, (int) mEstimates.size());

    unique_ptr<ceres::LossFunction> robustLoss(
        new ceres::HuberLoss(0.000001));

    for (int iter = 0; iter < max_iters; iter++) {
        ceres::Problem problem(pOptions);

        double m = mEstimates[iter];
        double b = 0;

        for (const auto& cf : costFunctions) {
            problem.AddResidualBlock(
                    cf.get(),
                    robustLoss.get(),
                    &m,
                    &b);
        }

        ceres::Solver::Options options;
        // Use default solver
        // options.linear_solver_type = ???;
        options.max_num_iterations = 100;
        options.minimizer_progress_to_stdout = false;

        ceres::Solver::Summary summary;
        ceres::Solve(options, &problem, &summary);

        double finalCost = summary.final_cost;

        if (finalCost < optimalCost) {
            optimalCost = finalCost;
            optimalM = m;
            optimalB = b;
        }
    }

    for (double& d : newValue) {
        d = d * optimalM + optimalB;
    }
}

void TriQPBO::computeFusion() {
    
}

void TriQPBO::initDelaunay() {
    adjacency.resize(points.size());

    edgeCount = 0;

    voronoi_diagram<double> vd;

    construct_voronoi(points.begin(), points.end(), &vd);

    printf("num cells = %d\n\n", vd.num_cells());

    vector<size_t> connectedPoints;

    // Loop over all vertices of the voronoi diagram, and all of the edges
    // radiating from each vertex.  The cells associated with each of these
    // edges are the cells which should be connected by a triangle.
    // If there are more than 3 such edges, two triangles are necessary.
    for (auto it = vd.vertices().begin(); it != vd.vertices().end(); ++it) {
        const auto& v = *it;

        connectedPoints.clear();

        const auto* edge = v.incident_edge();

        do {
            if (edge->is_primary()) {
                size_t pointIdx = edge->cell()->source_index();

                connectedPoints.push_back(pointIdx);
            }

            edge = edge->rot_next();

        } while (edge != v.incident_edge());

        // Iterate over each set of three consecutive points, with overlap
        // Note that this essentially constructs a triangle-fan from
        // the connected points.
        for (int i = 0; i < connectedPoints.size() - 1; i+=2) {
            size_t a = connectedPoints[i];
            size_t b = connectedPoints[(i + 1) % connectedPoints.size()];
            size_t c = connectedPoints[(i + 2) % connectedPoints.size()];

            triangles.push_back({a, b, c});

            size_t pairs[3][2] = {
                {a, b},
                {b, c},
                {c, a}
            };

            for (int p = 0; p < 3; p++) {
                size_t i0 = min(pairs[p][0], pairs[p][1]);
                size_t i1 = max(pairs[p][0], pairs[p][1]);

                if (adjacency[i0].count(i1) == 0) {
                    adjacency[i0][i1] = edgeCount;

                    edgeCount++;
                }
            }
        }
    }
}

void TriQPBO::initGModel() {
    gModelData = new GModelData();   

    GModelData::Space space(points.size(), 2);

    gModelData->model = GModelData::GModel(space);
}

