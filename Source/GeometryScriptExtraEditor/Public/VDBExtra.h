// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "ProxyLOD/Private/ProxyLODMeshSDFConversions.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "MeshDescription.h"
#include "MeshDescriptionToDynamicMesh.h"
#include "UDynamicMesh.h"
#include "VDBExtra.generated.h"


/* 
*	Function library class.
*	Each function in it is expected to be static and represents blueprint node that can be called in any blueprint.
*
*	When declaring function you can define metadata for the node. Key function specifiers will be BlueprintPure and BlueprintCallable.
*	BlueprintPure - means the function does not affect the owning object in any way and thus creates a node without Exec pins.
*	BlueprintCallable - makes a function which can be executed in Blueprints - Thus it has Exec pins.
*	DisplayName - full name of the node, shown when you mouse over the node and in the blueprint drop down menu.
*				Its lets you name the node using characters not allowed in C++ function names.
*	CompactNodeTitle - the word(s) that appear on the node.
*	Keywords -	the list of keywords that helps you to find node when you search for it using Blueprint drop-down menu. 
*				Good example is "Print String" node which you can find also by using keyword "log".
*	Category -	the category your node will be under in the Blueprint drop-down menu.
*
*	For more info on custom blueprint nodes visit documentation:
*	https://wiki.unrealengine.com/Custom_Blueprint_Node_Creation
*/

USTRUCT(BlueprintType, meta = (DisplayName = "ParticlesRasterize"))
struct GEOMETRYSCRIPTEXTRAEDITOR_API FParticleRasterize
{
    GENERATED_BODY()
public:
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
    FVector Position;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
    float Rad;
    
};


UCLASS()
class GEOMETRYSCRIPTEXTRAEDITOR_API UVDBExtra : public UBlueprintFunctionLibrary
{
	GENERATED_UCLASS_BODY()
	UFUNCTION(BlueprintCallable, Category = "VDBExtra")
	static UDynamicMesh* ParticlesToVDBMesh(UDynamicMesh* TargetMesh, TArray<FParticleRasterize> Particles, float VoxelSize = 5);
    
    UFUNCTION(BlueprintCallable, Category = "VDBExtra")
    static UDynamicMesh* ParticlesToVDBMeshUniform(UDynamicMesh* TargetMesh, TArray<FVector> Locations, float RadiusMult = 2, float VoxelSize = 5, bool PostProcess = false);
    
    static void ConvertMeshVDBExtra(openvdb::FloatGrid::ConstPtr SDFVolume, FMeshDescription& OutRawMesh);

    static void FixConvertMesh(UDynamicMesh* TargetMesh, UDynamicMesh* SourceMesh);
    
};


class VDBParticleList
{
protected:
    struct VDBParticle {
        openvdb::Vec3R p;
        openvdb::Vec3R v;
        openvdb::Real  r;
    };
    openvdb::Real           mRadiusScale;
    openvdb::Real           mVelocityScale;
    std::vector<VDBParticle> mParticleList;
public:
    using Real = openvdb::Real;
    using PosType = openvdb::Vec3R; // required by openvdb::tools::PointPartitioner

    VDBParticleList(Real radiusMult = 1, Real velocityMult = 1)
    : mRadiusScale(radiusMult), mVelocityScale(velocityMult)
    {
    }

    void Append(TArray<FParticleRasterize> Particles)
    {
        for (int i = 0; i < Particles.Num(); ++i)
        {
            VDBParticle pa;
            pa.p = openvdb::Vec3R(Particles[i].Position.X,Particles[i].Position.Y, Particles[i].Position.Z);
            pa.r = Particles[i].Rad;
            mParticleList.push_back(pa);
        }
    }
    
    void Add(FVector Pos, float Rad)
    {
        VDBParticle pa;
        pa.p = openvdb::Vec3R(Pos.X, Pos.Y, Pos.Z);
        pa.r = Rad;
        mParticleList.push_back(pa);
    }
    
    /// @return coordinate bbox in the space of the specified transform
    openvdb::CoordBBox getBBox(const openvdb::GridBase& grid)
    {
        openvdb::CoordBBox bbox;
        openvdb::Coord &min= bbox.min(), &max = bbox.max();
        openvdb::Vec3R pos;
        openvdb::Real rad, invDx = 1/grid.voxelSize()[0];
        for (size_t n=0, e=this->size(); n<e; ++n)
        {
            this->getPosRad(n, pos, rad);
            const openvdb::Vec3d xyz = grid.worldToIndex(pos);
            const openvdb::Real   r  = rad * invDx;
            for (int i=0; i<3; ++i)
            {
                min[i] = openvdb::math::Min(min[i], openvdb::math::Floor(xyz[i] - r));
                max[i] = openvdb::math::Max(max[i], openvdb::math::Ceil( xyz[i] + r));
            }
        }
        return bbox;
    }
    //typedef int AttributeType;
    // The methods below are only required for the unit-tests
    openvdb::Vec3R pos(int n)   const {return mParticleList[n].p;}
    openvdb::Vec3R vel(int n)   const {return openvdb::Vec3R(1, 1, 1);}
    openvdb::Real radius(int n) const {return mRadiusScale*mParticleList[n].r;}

    //////////////////////////////////////////////////////////////////////////////
    /// The methods below are the only ones required by tools::ParticleToLevelSet
    /// @note We return by value since the radius and velocities are modified
    /// by the scaling factors! Also these methods are all assumed to
    /// be thread-safe.

    /// Return the total number of particles in list.
    ///  Always required!
    size_t size() const { return mParticleList.size(); }

    /// Get the world space position of n'th particle.
    /// Required by ParticledToLevelSet::rasterizeSphere(*this,radius).
    void getPos(size_t n,  openvdb::Vec3R&pos) const { pos = mParticleList[n].p; }


    void getPosRad(size_t n,  openvdb::Vec3R& pos, openvdb::Real& rad) const {
        pos = mParticleList[n].p;
        rad = mRadiusScale*mParticleList[n].r;
    }
    void getPosRadVel(size_t n,  openvdb::Vec3R& pos, openvdb::Real& rad, openvdb::Vec3R& vel) const {
        pos = mParticleList[n].p;
        rad = mRadiusScale*mParticleList[n].r;
        vel = openvdb::Vec3R(1, 1, 1);
    }
    // The method below is only required for attribute transfer
    void getAtt(size_t n, openvdb::Index32& att) const { att = openvdb::Index32(n); }
}; // class ParticleList
