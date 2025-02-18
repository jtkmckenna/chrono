// =============================================================================
// PROJECT CHRONO - http://projectchrono.org
//
// Copyright (c) 2014 projectchrono.org
// All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be found
// in the LICENSE file at the top level of the distribution and at
// http://projectchrono.org/license-chrono.txt.
//
// =============================================================================
// Author: Radu Serban
// =============================================================================
//
// Chrono::Vehicle + Chrono::Multicore demo program for simulating a HMMWV vehicle
// over rigid or granular material.
//
// Contact uses the SMC (penalty) formulation.
//
// The global reference frame has Z up.
// All units SI.
// =============================================================================

#include <cstdio>
#include <cmath>
#include <vector>

#include "chrono/utils/ChUtilsInputOutput.h"

#include "chrono_vehicle/ChVehicleModelData.h"
#include "chrono_vehicle/ChDriver.h"
#include "chrono_vehicle/terrain/SCMTerrain.h"
#include "chrono_vehicle/terrain/RigidTerrain.h"

#include "chrono_models/vehicle/hmmwv/HMMWV.h"

#include "chrono_thirdparty/filesystem/path.h"

#ifdef CHRONO_POSTPROCESS
    #include "chrono_postprocess/ChGnuPlot.h"
#endif

#ifdef CHRONO_IRRLICHT
    #include "chrono_vehicle/wheeled_vehicle/ChWheeledVehicleVisualSystemIrrlicht.h"
using namespace chrono::irrlicht;
#endif

#ifdef CHRONO_VSG
    #include "chrono_vehicle/wheeled_vehicle/ChWheeledVehicleVisualSystemVSG.h"
using namespace chrono::vsg3d;
#endif

using namespace chrono;
using namespace chrono::vehicle;
using namespace chrono::vehicle::hmmwv;

using std::cout;
using std::endl;

// =============================================================================
// USER SETTINGS
// =============================================================================

// Run-time visualization system (IRRLICHT or VSG)
ChVisualSystem::Type vis_type = ChVisualSystem::Type::VSG;

// -----------------------------------------------------------------------------
// Terrain parameters
// -----------------------------------------------------------------------------

// If true, set the SCM terrain profile from a mesh (bump.obj).
// Otherwise, create a flat SCM terrain patch of given dimensions.
enum class PatchType { FLAT, MESH, HEIGHMAP };
PatchType patch_type = PatchType::HEIGHMAP;

double delta = 0.05;  // SCM grid spacing

// SCM terrain visualization options
bool render_wireframe = true;  // render wireframe (flat otherwise)
bool apply_texture = false;    // add texture
bool render_sinkage = true;    // use false coloring for sinkage visualization

// -----------------------------------------------------------------------------
// Vehicle parameters
// -----------------------------------------------------------------------------

// Type of tire (controls both contact and visualization)
enum class TireType { CYLINDRICAL, LUGGED };
TireType tire_type = TireType::LUGGED;

// Tire contact material properties
float Y_t = 1.0e6f;
float cr_t = 0.1f;
float mu_t = 0.8f;

// -----------------------------------------------------------------------------
// Simulation parameters
// -----------------------------------------------------------------------------

// Simulation step size
double step_size = 3e-3;

// Time interval between two render frames (1/FPS)
double render_step_size = 1.0 / 100;

// Point on chassis tracked by the camera
ChVector<> track_point(0.0, 0.0, 1.75);

// Output directories
const std::string out_dir = GetChronoOutputPath() + "HMMWV_DEF_SOIL";
const std::string img_dir = out_dir + "/IMG";

// Visualization output
bool img_output = false;

// =============================================================================

class MyDriver : public ChDriver {
  public:
    MyDriver(ChVehicle& vehicle, double delay) : ChDriver(vehicle), m_delay(delay) {}
    ~MyDriver() {}

    virtual void Synchronize(double time) override {
        m_throttle = 0;
        m_steering = 0;
        m_braking = 0;

        double eff_time = time - m_delay;

        // Do not generate any driver inputs for a duration equal to m_delay.
        if (eff_time < 0)
            return;

        if (eff_time > 0.2)
            m_throttle = 0.7;
        else
            m_throttle = 3.5 * eff_time;

        if (eff_time < 2)
            m_steering = 0;
        else
            m_steering = 0.6 * std::sin(CH_C_2PI * (eff_time - 2) / 6);
    }

  private:
    double m_delay;
};

// =============================================================================

void CreateLuggedGeometry(std::shared_ptr<ChBody> wheel_body, std::shared_ptr<ChMaterialSurfaceSMC> wheel_material) {
    std::string lugged_file("hmmwv/lugged_wheel_section.obj");
    geometry::ChTriangleMeshConnected lugged_mesh;
    ChConvexDecompositionHACDv2 lugged_convex;
    chrono::utils::LoadConvexMesh(vehicle::GetDataFile(lugged_file), lugged_mesh, lugged_convex);
    int num_hulls = lugged_convex.GetHullCount();

    // Assemble the tire contact from 15 segments, properly offset.
    // Each segment is further decomposed in convex hulls.
    for (int iseg = 0; iseg < 15; iseg++) {
        ChQuaternion<> rot = Q_from_AngAxis(iseg * 24 * CH_C_DEG_TO_RAD, VECT_Y);
        for (int ihull = 0; ihull < num_hulls; ihull++) {
            std::vector<ChVector<> > convexhull;
            lugged_convex.GetConvexHullResult(ihull, convexhull);
            auto shape = chrono_types::make_shared<ChCollisionShapeConvexHull>(wheel_material, convexhull);
            wheel_body->AddCollisionShape(shape, ChFrame<>(VNULL, rot));
        }
    }

    // Add a cylinder to represent the wheel hub.
    auto cyl_shape = chrono_types::make_shared<ChCollisionShapeCylinder>(wheel_material, 0.223, 0.252);
    wheel_body->AddCollisionShape(cyl_shape, ChFrame<>(VNULL, Q_from_AngX(CH_C_PI_2)));

    // Visualization
    auto trimesh = geometry::ChTriangleMeshConnected::CreateFromWavefrontFile(
        vehicle::GetDataFile("hmmwv/lugged_wheel.obj"), false, false);

    auto trimesh_shape = chrono_types::make_shared<ChVisualShapeTriangleMesh>();
    trimesh_shape->SetMesh(trimesh);
    trimesh_shape->SetMutable(false);
    trimesh_shape->SetName("lugged_wheel");
    trimesh_shape->SetColor(ChColor(0.3f, 0.3f, 0.3f));
    wheel_body->AddVisualShape(trimesh_shape);
}

// =============================================================================

int main(int argc, char* argv[]) {
    GetLog() << "Copyright (c) 2017 projectchrono.org\nChrono version: " << CHRONO_VERSION << "\n\n";

    // Set initial vehicle location
    ChVector<> init_loc;
    ChVector2<> patch_size;
    switch (patch_type) {
        case PatchType::FLAT:
            init_loc = ChVector<>(-5.0, -2.0, 0.6);
            patch_size = ChVector2<>(16.0, 8.0);
            break;
        case PatchType::MESH:
            init_loc = ChVector<>(-12.0, -12.0, 1.6);
            break;
        case PatchType::HEIGHMAP:
            init_loc = ChVector<>(-15.0, -15.0, 0.6);
            patch_size = ChVector2<>(40.0, 40.0);
            break;
    }

    // --------------------
    // Create HMMWV vehicle
    // --------------------
    HMMWV_Full hmmwv;
    hmmwv.SetCollisionSystemType(ChCollisionSystem::Type::BULLET);
    hmmwv.SetContactMethod(ChContactMethod::SMC);
    hmmwv.SetChassisFixed(false);
    hmmwv.SetInitPosition(ChCoordsys<>(init_loc, QUNIT));
    hmmwv.SetEngineType(EngineModelType::SHAFTS);
    hmmwv.SetTransmissionType(TransmissionModelType::SHAFTS);
    hmmwv.SetDriveType(DrivelineTypeWV::AWD);
    switch (tire_type) {
        case TireType::CYLINDRICAL:
            hmmwv.SetTireType(TireModelType::RIGID_MESH);
            break;
        case TireType::LUGGED:
            hmmwv.SetTireType(TireModelType::RIGID);
            break;
    }
    hmmwv.Initialize();

    hmmwv.SetChassisVisualizationType(VisualizationType::NONE);

    // -----------------------------------------------------------
    // Set tire contact material, contact model, and visualization
    // -----------------------------------------------------------
    auto wheel_material = chrono_types::make_shared<ChMaterialSurfaceSMC>();
    wheel_material->SetFriction(mu_t);
    wheel_material->SetYoungModulus(Y_t);
    wheel_material->SetRestitution(cr_t);

    switch (tire_type) {
        case TireType::CYLINDRICAL:
            hmmwv.SetTireVisualizationType(VisualizationType::MESH);
            break;
        case TireType::LUGGED:
            hmmwv.SetTireVisualizationType(VisualizationType::NONE);
            for (auto& axle : hmmwv.GetVehicle().GetAxles()) {
                CreateLuggedGeometry(axle->m_wheels[0]->GetSpindle(), wheel_material);
                CreateLuggedGeometry(axle->m_wheels[1]->GetSpindle(), wheel_material);
            }
    }

    // --------------------
    // Create driver system
    // --------------------
    MyDriver driver(hmmwv.GetVehicle(), 0.5);
    driver.Initialize();

    // ------------------
    // Create the terrain
    // ------------------
    ChSystem* system = hmmwv.GetSystem();
    system->SetNumThreads(std::min(8, ChOMP::GetNumProcs()));

    SCMTerrain terrain(system);
    terrain.SetSoilParameters(2e6,   // Bekker Kphi
                              0,     // Bekker Kc
                              1.1,   // Bekker n exponent
                              0,     // Mohr cohesive limit (Pa)
                              30,    // Mohr friction limit (degrees)
                              0.01,  // Janosi shear coefficient (m)
                              2e8,   // Elastic stiffness (Pa/m), before plastic yield
                              3e4    // Damping (Pa s/m), proportional to negative vertical speed (optional)
    );

    // Optionally, enable bulldozing effects.
    ////terrain.EnableBulldozing(true);      // inflate soil at the border of the rut
    ////terrain.SetBulldozingParameters(55,   // angle of friction for erosion of displaced material at rut border
    ////                                0.8,  // displaced material vs downward pressed material.
    ////                                5,    // number of erosion refinements per timestep
    ////                                10);  // number of concentric vertex selections subject to erosion

    // Optionally, enable moving patch feature (single patch around vehicle chassis)
    terrain.AddMovingPatch(hmmwv.GetChassisBody(), ChVector<>(0, 0, 0), ChVector<>(5, 3, 1));

    // Optionally, enable moving patch feature (multiple patches around each wheel)
    ////for (auto& axle : hmmwv.GetVehicle().GetAxles()) {
    ////    terrain.AddMovingPatch(axle->m_wheels[0]->GetSpindle(), ChVector<>(0, 0, 0), ChVector<>(1, 0.5, 1));
    ////    terrain.AddMovingPatch(axle->m_wheels[1]->GetSpindle(), ChVector<>(0, 0, 0), ChVector<>(1, 0.5, 1));
    ////}

    switch (patch_type) {
        case PatchType::FLAT:
            terrain.Initialize(patch_size.x(), patch_size.y(), delta);
            break;
        case PatchType::MESH:
            terrain.Initialize(vehicle::GetDataFile("terrain/meshes/bump.obj"), delta);
            break;
        case PatchType::HEIGHMAP:
            terrain.Initialize(vehicle::GetDataFile("terrain/height_maps/bump64.bmp"), patch_size.x(), patch_size.y(),
                               0.0, 1.0, delta);
            break;
    }

    // Control visualization of SCM terrain
    terrain.GetMesh()->SetWireframe(render_wireframe);

    if (apply_texture)
        terrain.GetMesh()->SetTexture(vehicle::GetDataFile("terrain/textures/dirt.jpg"));

    if (render_sinkage) {
        terrain.SetPlotType(vehicle::SCMTerrain::PLOT_SINKAGE, 0, 0.1);
        ////terrain.SetPlotType(vehicle::SCMTerrain::PLOT_PRESSURE_YELD, 0, 30000.2);
    }

    // -------------------------------------------
    // Create the run-time visualization interface
    // -------------------------------------------

#ifndef CHRONO_IRRLICHT
    if (vis_type == ChVisualSystem::Type::IRRLICHT)
        vis_type = ChVisualSystem::Type::VSG;
#endif
#ifndef CHRONO_VSG
    if (vis_type == ChVisualSystem::Type::VSG)
        vis_type = ChVisualSystem::Type::IRRLICHT;
#endif

    std::shared_ptr<ChVehicleVisualSystem> vis;
    switch (vis_type) {
        case ChVisualSystem::Type::IRRLICHT: {
#ifdef CHRONO_IRRLICHT
            auto vis_irr = chrono_types::make_shared<ChWheeledVehicleVisualSystemIrrlicht>();
            vis_irr->SetWindowTitle("Wheeled vehicle on SCM deformable terrain");
            vis_irr->SetChaseCamera(track_point, 6.0, 0.5);
            vis_irr->Initialize();
            vis_irr->AddLightDirectional();
            vis_irr->AddSkyBox();
            vis_irr->AddLogo();
            vis_irr->AttachVehicle(&hmmwv.GetVehicle());

            vis = vis_irr;
#endif
            break;
        }
        default:
        case ChVisualSystem::Type::VSG: {
#ifdef CHRONO_VSG
            auto vis_vsg = chrono_types::make_shared<ChWheeledVehicleVisualSystemVSG>();
            vis_vsg->SetWindowTitle("Wheeled vehicle on SCM deformable terrain");
            vis_vsg->SetWindowSize(ChVector2<int>(1000, 800));
            vis_vsg->SetWindowPosition(ChVector2<int>(100, 100));
            vis_vsg->SetUseSkyBox(true);
            vis_vsg->SetCameraAngleDeg(40);
            vis_vsg->SetLightIntensity(1.0f);
            vis_vsg->SetChaseCamera(track_point, 10.0, 0.5);
            vis_vsg->AttachVehicle(&hmmwv.GetVehicle());
            vis_vsg->AddGuiColorbar("Sinkage (m)", 0.0, 0.1);
            vis_vsg->Initialize();

            vis = vis_vsg;
#endif
            break;
        }
    }

    // -----------------
    // Initialize output
    // -----------------
    if (!filesystem::create_directory(filesystem::path(out_dir))) {
        std::cout << "Error creating directory " << out_dir << std::endl;
        return 1;
    }
    if (img_output) {
        if (!filesystem::create_directory(filesystem::path(img_dir))) {
            std::cout << "Error creating directory " << img_dir << std::endl;
            return 1;
        }
    }

    // ---------------
    // Simulation loop
    // ---------------
    std::cout << "Total vehicle mass: " << hmmwv.GetVehicle().GetMass() << std::endl;

    // Solver settings.
    system->SetSolverMaxIterations(50);

    // Number of simulation steps between two 3D view render frames
    int render_steps = (int)std::ceil(render_step_size / step_size);

    // Initialize simulation frame counter
    int step_number = 0;
    int render_frame = 0;

    ChTimer timer;

    while (vis->Run()) {
        double time = system->GetChTime();

        if (step_number == 800) {
            std::cout << "\nstart timer at t = " << time << std::endl;
            timer.start();
        }
        if (step_number == 1400) {
            timer.stop();
            std::cout << "stop timer at t = " << time << std::endl;
            std::cout << "elapsed: " << timer() << std::endl;
            std::cout << "\nSCM stats for last step:" << std::endl;
            terrain.PrintStepStatistics(std::cout);
        }

        // Render scene
        vis->BeginScene();
        vis->Render();
        ////tools::drawColorbar(vis.get(), 0, 0.1, "Sinkage", 30, 200);
        vis->EndScene();

        if (img_output && step_number % render_steps == 0) {
            std::ostringstream filename;
            filename << img_dir
                     << "/img_"
                     // Frame number is zero padded for nicer alphabetical file sorting
                     // Is 3 digits enough space for all the frames?
                     << std::setw(3) << std::setfill('0') << render_frame + 1 << ".jpg";
            vis->WriteImageToFile(filename.str());
            render_frame++;
        }

        // Driver inputs
        DriverInputs driver_inputs = driver.GetInputs();

        // Update modules
        driver.Synchronize(time);
        terrain.Synchronize(time);
        hmmwv.Synchronize(time, driver_inputs, terrain);
        vis->Synchronize(time, driver_inputs);

        // Advance dynamics
        hmmwv.Advance(step_size);
        vis->Advance(step_size);

        // Increment frame number
        step_number++;
    }

    return 0;
}
