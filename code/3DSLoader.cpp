/*
---------------------------------------------------------------------------
Open Asset Import Library (ASSIMP)
---------------------------------------------------------------------------

Copyright (c) 2006-2008, ASSIMP Development Team

All rights reserved.

Redistribution and use of this software in source and binary forms, 
with or without modification, are permitted provided that the following 
conditions are met:

* Redistributions of source code must retain the above
  copyright notice, this list of conditions and the
  following disclaimer.

* Redistributions in binary form must reproduce the above
  copyright notice, this list of conditions and the
  following disclaimer in the documentation and/or other
  materials provided with the distribution.

* Neither the name of the ASSIMP team, nor the names of its
  contributors may be used to endorse or promote products
  derived from this software without specific prior
  written permission of the ASSIMP Development Team.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS 
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT 
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT 
OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT 
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY 
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT 
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE 
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
---------------------------------------------------------------------------
*/

/** @file Implementation of the 3ds importer class */

// internal headers
#include "3DSLoader.h"
#include "MaterialSystem.h"
#include "TextureTransform.h"
#include "StringComparison.h"
#include "qnan.h"

// public ASSIMP headers
#include "../include/DefaultLogger.h"
#include "../include/aiScene.h"
#include "../include/aiAssert.h"
#include "../include/IOStream.h"
#include "../include/IOSystem.h"
#include "../include/assimp.hpp"

// boost headers
#include <boost/scoped_ptr.hpp>

using namespace Assimp;
		

// begin a chunk: parse it, validate its length, get a pointer to its end
#define ASSIMP_3DS_BEGIN_CHUNK() \
	const Dot3DSFile::Chunk* psChunk; \
	this->ReadChunk(&psChunk); \
	const unsigned char* pcCur = this->mCurrent; \
	const unsigned char* pcCurNext = pcCur + (psChunk->Size \
		- sizeof(Dot3DSFile::Chunk));

// process the end of a chunk and return if the end of the file is reached
#define ASSIMP_3DS_END_CHUNK() \
	this->mCurrent = pcCurNext; \
	piRemaining -= psChunk->Size; \
	if (0 >= piRemaining)return;


// check whether the size of all subordinate chunks of a chunks is
// not larger than the size of the chunk itself
#ifdef _DEBUG
#	define ASSIMP_3DS_WARN_CHUNK_OVERFLOW_MSG \
		"Size of chunk data plus size of subordinate chunks is " \
		"larger than the size specified in the top-level chunk header."	

#	define ASSIMP_3DS_VALIDATE_CHUNK_SIZE() \
	if (pcCurNext < this->mCurrent) \
	{ \
		DefaultLogger::get()->warn(ASSIMP_3DS_WARN_CHUNK_OVERFLOW_MSG); \
		pcCurNext = this->mCurrent; \
	}
#else
#	define ASSIMP_3DS_VALIDATE_CHUNK_SIZE()
#endif

// ------------------------------------------------------------------------------------------------
// Constructor to be privately used by Importer
Dot3DSImporter::Dot3DSImporter()
{
}

// ------------------------------------------------------------------------------------------------
// Destructor, private as well 
Dot3DSImporter::~Dot3DSImporter()
{
}

// ------------------------------------------------------------------------------------------------
// Returns whether the class can handle the format of the given file. 
bool Dot3DSImporter::CanRead( const std::string& pFile, IOSystem* pIOHandler) const
{
	// simple check of file extension is enough for the moment
	std::string::size_type pos = pFile.find_last_of('.');
	// no file extension - can't read
	if( pos == std::string::npos)
		return false;
	std::string extension = pFile.substr( pos);

	// not brillant but working ;-)
	if( extension == ".3ds" || extension == ".3DS" || 
		extension == ".3Ds" || extension == ".3dS")
		return true;

	return false;
}
// ------------------------------------------------------------------------------------------------
// Setup configuration properties
void Dot3DSImporter::SetupProperties(const Importer* pImp)
{
	this->configSkipPivot = pImp->GetPropertyInteger(AI_CONFIG_IMPORT_3DS_IGNORE_PIVOT,0) ? true : false;
}
// ------------------------------------------------------------------------------------------------
// Imports the given file into the given scene structure. 
void Dot3DSImporter::InternReadFile( 
	const std::string& pFile, aiScene* pScene, IOSystem* pIOHandler)
{
	boost::scoped_ptr<IOStream> file( pIOHandler->Open( pFile));

	// Check whether we can read from the file
	if( file.get() == NULL)
		throw new ImportErrorException( "Failed to open 3DS file " + pFile + ".");

	// check whether the .3ds file is large enough to contain
	// at least one chunk.
	size_t fileSize = file->FileSize();
	if( fileSize < 16)
		throw new ImportErrorException( "3DS File is too small.");

	this->mScene = new Dot3DS::Scene();

	// allocate storage and copy the contents of the file to a memory buffer
	std::vector<unsigned char> mBuffer2(fileSize);
	file->Read( &mBuffer2[0], 1, fileSize);
	this->mCurrent = this->mBuffer = &mBuffer2[0];
	this->mLast = this->mBuffer+fileSize;

	// initialize members
	this->mLastNodeIndex = -1;
	this->mCurrentNode = new Dot3DS::Node();
	this->mRootNode = this->mCurrentNode;
	this->mRootNode->mHierarchyPos = -1;
	this->mRootNode->mHierarchyIndex = -1;
	this->mRootNode->mParent = NULL;
	this->mMasterScale = 1.0f;
	this->mBackgroundImage = "";
	this->bHasBG = false;

	int iRemaining = (unsigned int)fileSize;
	this->ParseMainChunk(iRemaining);

	// Generate an unique set of vertices/indices for
	// all meshes contained in the file
	for (std::vector<Dot3DS::Mesh>::iterator
		i =  this->mScene->mMeshes.begin();
		i != this->mScene->mMeshes.end();++i)
	{
		// TODO: see function body
		this->CheckIndices(*i);
		this->MakeUnique(*i);

		// first generate normals for the mesh
		ComputeNormalsWithSmoothingsGroups<Dot3DS::Face>(*i);
	}

	// Apply scaling and offsets to all texture coordinates
	TextureTransform::ApplyScaleNOffset(this->mScene->mMaterials);

	// Replace all occurences of the default material with a valid material.
	// Generate it if no material containing DEFAULT in its name has been
	// found in the file
	this->ReplaceDefaultMaterial();

	// Convert the scene from our internal representation to an aiScene object
	this->ConvertScene(pScene);

	// Generate the node graph for the scene. This is a little bit
	// tricky since we'll need to split some meshes into submeshes
	this->GenerateNodeGraph(pScene);

	// Now apply a master scaling factor to the scene
	this->ApplyMasterScale(pScene);
}
// ------------------------------------------------------------------------------------------------
void Dot3DSImporter::ApplyMasterScale(aiScene* pScene)
{
	if (!this->mMasterScale)this->mMasterScale = 1.0f;
	else this->mMasterScale = 1.0f / this->mMasterScale;

	// construct an uniform scaling matrix and multiply with it
	pScene->mRootNode->mTransformation *= aiMatrix4x4( 
		this->mMasterScale,0.0f, 0.0f, 0.0f,
		0.0f, this->mMasterScale,0.0f, 0.0f,
		0.0f, 0.0f, this->mMasterScale,0.0f,
		0.0f, 0.0f, 0.0f, 1.0f);
}
// ------------------------------------------------------------------------------------------------
void Dot3DSImporter::ReadChunk(const Dot3DSFile::Chunk** p_ppcOut)
{
	ai_assert(p_ppcOut != NULL);

	// read chunk
	if (this->mCurrent >= this->mLast)
		throw new ImportErrorException("Unexpected end of file, can't read chunk header");

	const uintptr_t iDiff = this->mLast - this->mCurrent;
	if (iDiff < sizeof(Dot3DSFile::Chunk)) 
	{
		*p_ppcOut = NULL;
		return;
	}
	*p_ppcOut = (const Dot3DSFile::Chunk*) this->mCurrent;
	if ((**p_ppcOut).Size + this->mCurrent > this->mLast)
		throw new ImportErrorException("Unexpected end of file, can't read chunk footer");

	this->mCurrent += sizeof(Dot3DSFile::Chunk);
	return;
}
// ------------------------------------------------------------------------------------------------
void Dot3DSImporter::ParseMainChunk(int& piRemaining)
{
	ASSIMP_3DS_BEGIN_CHUNK();

	// get chunk type
	int iRemaining = (psChunk->Size - sizeof(Dot3DSFile::Chunk));
	switch (psChunk->Flag)
	{
	case Dot3DSFile::CHUNK_MAIN:
		this->ParseEditorChunk(iRemaining);
		break;
	};
	ASSIMP_3DS_VALIDATE_CHUNK_SIZE();
	ASSIMP_3DS_END_CHUNK();
	return this->ParseMainChunk(piRemaining);
}
// ------------------------------------------------------------------------------------------------
void Dot3DSImporter::ParseEditorChunk(int& piRemaining)
{
	ASSIMP_3DS_BEGIN_CHUNK();

	// get chunk type
	int iRemaining = (psChunk->Size - sizeof(Dot3DSFile::Chunk));
	switch (psChunk->Flag)
	{
	case Dot3DSFile::CHUNK_OBJMESH:

		this->ParseObjectChunk(iRemaining);
		break;

	// NOTE: In several documentations in the internet this
	// chunk appears at different locations
	case Dot3DSFile::CHUNK_KEYFRAMER:

		this->ParseKeyframeChunk(iRemaining);
		break;

	case Dot3DSFile::CHUNK_VERSION:

		if (psChunk->Size >= 2+sizeof(Dot3DSFile::Chunk))
		{
			// print the version number
			char szBuffer[128];
			::sprintf(szBuffer,"3DS file version chunk: %i",
				(int) *((uint16_t*)this->mCurrent));
			DefaultLogger::get()->info(szBuffer);
		}
		else
		{
			DefaultLogger::get()->warn("Invalid version chunk in 3DS file");
		}
		break;
	};
	ASSIMP_3DS_VALIDATE_CHUNK_SIZE();
	ASSIMP_3DS_END_CHUNK();
	return this->ParseEditorChunk(piRemaining);
}
// ------------------------------------------------------------------------------------------------
void Dot3DSImporter::ParseObjectChunk(int& piRemaining)
{
	ASSIMP_3DS_BEGIN_CHUNK();

	const unsigned char* sz = this->mCurrent;
	unsigned int iCnt = 0;

	// get chunk type
	int iRemaining = (psChunk->Size - sizeof(Dot3DSFile::Chunk));
	switch (psChunk->Flag)
	{
	case Dot3DSFile::CHUNK_OBJBLOCK:

		this->mScene->mMeshes.push_back(Dot3DS::Mesh());

		// at first we need to parse the name of the
		// geometry object

		while (*sz++ != '\0')
		{
			if (sz > pcCurNext-1)break;
			++iCnt;
		}
		this->mScene->mMeshes.back().mName = std::string(
			(const char*)this->mCurrent,iCnt);
		++iCnt;

		this->mCurrent += iCnt;
		iRemaining -= iCnt;
		this->ParseChunk(iRemaining);
		break;

	case Dot3DSFile::CHUNK_MAT_MATERIAL:

		this->mScene->mMaterials.push_back(Dot3DS::Material());
		this->ParseMaterialChunk(iRemaining);
		break;

	case Dot3DSFile::CHUNK_AMBCOLOR:

		// This is the ambient base color of the scene.
		// We add it to the ambient color of all materials
		this->ParseColorChunk(&this->mClrAmbient,true);
		if (is_qnan(this->mClrAmbient.r))
			{
			this->mClrAmbient.r = 0.0f;
			this->mClrAmbient.g = 0.0f;
			this->mClrAmbient.b = 0.0f;
			}
		break;

	case Dot3DSFile::CHUNK_BIT_MAP:
		this->mBackgroundImage = std::string((const char*)this->mCurrent);
		break;


	case Dot3DSFile::CHUNK_BIT_MAP_EXISTS:
		bHasBG = true;
		break;


	case Dot3DSFile::CHUNK_MASTER_SCALE:

		this->mMasterScale = *((float*)this->mCurrent);
		this->mCurrent += sizeof(float);
		break;

	// NOTE: In several documentations in the internet this
	// chunk appears at different locations
	case Dot3DSFile::CHUNK_KEYFRAMER:

		this->ParseKeyframeChunk(iRemaining);
		break;

	};
	ASSIMP_3DS_VALIDATE_CHUNK_SIZE();
	ASSIMP_3DS_END_CHUNK();
	return this->ParseObjectChunk(piRemaining);
}
// ------------------------------------------------------------------------------------------------
void Dot3DSImporter::SkipChunk()
{
	const Dot3DSFile::Chunk* psChunk;
	this->ReadChunk(&psChunk);
	
	this->mCurrent += psChunk->Size - sizeof(Dot3DSFile::Chunk);
	return;
}
// ------------------------------------------------------------------------------------------------
void Dot3DSImporter::ParseChunk(int& piRemaining)
{
	ASSIMP_3DS_BEGIN_CHUNK();

	// get chunk type
	int iRemaining = (psChunk->Size - sizeof(Dot3DSFile::Chunk));
	switch (psChunk->Flag)
	{
	case Dot3DSFile::CHUNK_TRIMESH:
		// this starts a new mesh
		this->ParseMeshChunk(iRemaining);
		break;
	};
	ASSIMP_3DS_VALIDATE_CHUNK_SIZE();
	ASSIMP_3DS_END_CHUNK();
	return this->ParseChunk(piRemaining);
}
// ------------------------------------------------------------------------------------------------
void Dot3DSImporter::ParseKeyframeChunk(int& piRemaining)
{
	ASSIMP_3DS_BEGIN_CHUNK();

	// get chunk type
	int iRemaining = (psChunk->Size - sizeof(Dot3DSFile::Chunk));
	switch (psChunk->Flag)
	{
	case Dot3DSFile::CHUNK_TRACKINFO:
		// this starts a new mesh
		this->ParseHierarchyChunk(iRemaining);
		break;
	};
	ASSIMP_3DS_VALIDATE_CHUNK_SIZE();
	ASSIMP_3DS_END_CHUNK();
	return this->ParseKeyframeChunk(piRemaining);
}
// ------------------------------------------------------------------------------------------------
void Dot3DSImporter::InverseNodeSearch(Dot3DS::Node* pcNode,Dot3DS::Node* pcCurrent)
{
	if (NULL == pcCurrent)
	{
		this->mRootNode->push_back(pcNode);
		return;
	}
	if (pcCurrent->mHierarchyPos == pcNode->mHierarchyPos)
	{
		if(pcCurrent->mParent)pcCurrent->mParent->push_back(pcNode);
		else pcCurrent->push_back(pcNode);
		return;
	}
	return this->InverseNodeSearch(pcNode,pcCurrent->mParent);
}
// ------------------------------------------------------------------------------------------------
void Dot3DSImporter::ParseHierarchyChunk(int& piRemaining)
{
	ASSIMP_3DS_BEGIN_CHUNK();

	// get chunk type
	const unsigned char* sz = (unsigned char*)this->mCurrent;
	unsigned int iCnt = 0;
	uint16_t iHierarchy;
	Dot3DS::Node* pcNode;
	switch (psChunk->Flag)
	{
	case Dot3DSFile::CHUNK_TRACKOBJNAME:
		
		// get object name
		while (*sz++ != '\0')
		{
			if (sz > pcCurNext-1)break;
			++iCnt;
		}
		pcNode = new Dot3DS::Node();
		pcNode->mName = std::string((const char*)this->mCurrent,iCnt);

		iCnt++;
		// there are two unknown values which we can safely ignore
		this->mCurrent += iCnt + sizeof(uint16_t)*2;
		iHierarchy = *((uint16_t*)this->mCurrent);
		iHierarchy++;
		pcNode->mHierarchyPos = iHierarchy;
		pcNode->mHierarchyIndex = this->mLastNodeIndex;
		if (this->mCurrentNode && this->mCurrentNode->mHierarchyPos == iHierarchy) 
		{
			// add to the parent of the last touched node
			this->mCurrentNode->mParent->push_back(pcNode);
			this->mLastNodeIndex++;	
		}
		else if(iHierarchy >= this->mLastNodeIndex)
		{
			// place it at the current position in the hierarchy
			this->mCurrentNode->push_back(pcNode);
			this->mLastNodeIndex = iHierarchy;
		}
		else
		{
			// need to go back to the specified position in the hierarchy.
			this->InverseNodeSearch(pcNode,this->mCurrentNode);
			this->mLastNodeIndex++;	
		}
		this->mCurrentNode = pcNode;
		break;

	case Dot3DSFile::CHUNK_TRACKPIVOT:

		// pivot = origin of rotation and scaling
		this->mCurrentNode->vPivot = *((const aiVector3D*)this->mCurrent);
		std::swap(this->mCurrentNode->vPivot.y,this->mCurrentNode->vPivot.z);
		this->mCurrent += sizeof(aiVector3D);
		break;

#ifdef AI_3DS_KEYFRAME_ANIMATION

	case Dot3DSFile::CHUNK_TRACKPOS:

		/*
		+2 short flags; 
		+8 short unknown[4];
		+2 short keys;
		+2 short unknown;
		struct {
		+2 short framenum;
		+4 long unknown;
		float pos_x, pos_y, pos_z; 
		}  pos[keys]; 	
		*/
		this->mCurrent += 10;
		iTemp = *((const uint16_t*)mCurrent);

		this->mCurrent += sizeof(uint16_t) * 2;

		for (unsigned int i = 0; i < (unsigned int)iTemp;++i)
		{
			uint16_t sNum = *((const uint16_t*)mCurrent);
			this->mCurrent += sizeof(uint16_t);

			aiVectorKey v;v.mTime = (double)sNum;

			this->mCurrent += sizeof(uint32_t);
			v.mValue =  *((const aiVector3D*)this->mCurrent);
			this->mCurrent += sizeof(aiVector3D);

			// check whether we do already have this keyframe
			for (std::vector<aiVectorKey>::const_iterator
				i =  this->mCurrentNode->aPositionKeys.begin();
				i != this->mCurrentNode->aPositionKeys.end();++i)
			{
				if ((*i).mTime == v.mTime){v.mTime = -10e10f;break;}
			}
			// add the new keyframe
			if (v.mTime != -10e10f)
				this->mCurrentNode->aPositionKeys.push_back(v);
		}
		break;

	case Dot3DSFile::CHUNK_TRACKROTATE:

		/*
		+2 short flags; 
		+8 short unknown[4];
		+2 short keys;
		+2 short unknown;
		struct {
		+2 short framenum;
		+4 long unknown;
		float rad , pos_x, pos_y, pos_z; 
		}  pos[keys]; 	
		*/
		this->mCurrent += 10;
		iTemp = *((const uint16_t*)mCurrent);

		this->mCurrent += sizeof(uint16_t) * 2;

		for (unsigned int i = 0; i < (unsigned int)iTemp;++i)
		{
			uint16_t sNum = *((const uint16_t*)mCurrent);
			this->mCurrent += sizeof(uint16_t);

			aiQuatKey v;v.mTime = (double)sNum;
			this->mCurrent += sizeof(uint32_t);

			float fRadians = *((const float*)this->mCurrent);
			this->mCurrent += sizeof(float);

			aiVector3D vAxis = *((const aiVector3D*)this->mCurrent);
			this->mCurrent += sizeof(aiVector3D);

			// construct a rotation quaternion from the axis-angle pair
			v.mValue = aiQuaternion(vAxis,fRadians);

			// check whether we do already have this keyframe
			for (std::vector<aiQuatKey>::const_iterator
				i =  this->mCurrentNode->aRotationKeys.begin();
				i != this->mCurrentNode->aRotationKeys.end();++i)
			{
				if ((*i).mTime == v.mTime){v.mTime = -10e10f;break;}
			}
			// add the new keyframe
			if (v.mTime != -10e10f)
				this->mCurrentNode->aRotationKeys.push_back(v);
		}
		break;

	case Dot3DSFile::CHUNK_TRACKSCALE:

		/*
		+2 short flags; 
		+8 short unknown[4];
		+2 short keys;
		+2 short unknown;
		struct {
		+2 short framenum;
		+4 long unknown;
		float pos_x, pos_y, pos_z; 
		}  pos[keys]; 	
		*/
		this->mCurrent += 10;
		iTemp = *((const uint16_t*)mCurrent);

		this->mCurrent += sizeof(uint16_t) * 2;
		for (unsigned int i = 0; i < (unsigned int)iTemp;++i)
		{
			uint16_t sNum = *((const uint16_t*)mCurrent);
			this->mCurrent += sizeof(uint16_t);

			aiVectorKey v;
			v.mTime = (double)sNum;

			this->mCurrent += sizeof(uint32_t);
			v.mValue =  *((const aiVector3D*)this->mCurrent);
			this->mCurrent += sizeof(aiVector3D);

			// check whether we do already have this keyframe
			for (std::vector<aiVectorKey>::const_iterator
				i =  this->mCurrentNode->aScalingKeys.begin();
				i != this->mCurrentNode->aScalingKeys.end();++i)
			{
				if ((*i).mTime == v.mTime){v.mTime = -10e10f;break;}
			}
			// add the new keyframe
			if (v.mTime != -10e10f)this->mCurrentNode->aScalingKeys.push_back(v);

			if (v.mValue.x && v.mValue.y && v.mValue.z)
			{
				DefaultLogger::get()->warn("Found zero scaled axis in scaling keyframe");
				++iCnt;
			}
		}
		// there are 3DS files that have only zero scalings
		if (iTemp == iCnt)
		{
			DefaultLogger::get()->warn("All scaling keys are zero. They will be removed");
			this->mCurrentNode->aScalingKeys.clear();
		}
		break;
#endif
	};
	ASSIMP_3DS_VALIDATE_CHUNK_SIZE();
	ASSIMP_3DS_END_CHUNK();
	return this->ParseHierarchyChunk(piRemaining);
}
// ------------------------------------------------------------------------------------------------
void Dot3DSImporter::ParseFaceChunk(int& piRemaining)
{
	ASSIMP_3DS_BEGIN_CHUNK();
	Dot3DS::Mesh& mMesh = this->mScene->mMeshes.back();

	// get chunk type
	const unsigned char* sz = this->mCurrent;
	uint32_t iCnt = 0,iTemp;
	switch (psChunk->Flag)
	{
	case Dot3DSFile::CHUNK_SMOOLIST:

		// one int32 for each face
		for (std::vector<Dot3DS::Face>::iterator
			i =  mMesh.mFaces.begin();
			i != mMesh.mFaces.end();++i)
		{
			// nth bit is set for nth smoothing group
			(*i).iSmoothGroup = *((uint32_t*)this->mCurrent);
			this->mCurrent += sizeof(uint32_t);
		}
		break;

	case Dot3DSFile::CHUNK_FACEMAT:

		// at fist an asciiz with the material name
		while (*sz++)
		{
			// make sure we don't run over the end of the chunk
			if (sz > pcCurNext-1)break;
		}

		// find the index of the material
		unsigned int iIndex = 0xFFFFFFFF;
		iCnt = 0;
		for (std::vector<Dot3DS::Material>::const_iterator
			i =  this->mScene->mMaterials.begin();
			i != this->mScene->mMaterials.end();++i,++iCnt)
		{
			// compare case-independent to be sure it works
			if (0 == ASSIMP_stricmp((const char*)this->mCurrent,
				(const char*)((*i).mName.c_str())))
			{
			iIndex = iCnt;
			break;
			}
		}
		if (0xFFFFFFFF == iIndex)
		{
			// this material is not known. Ignore this. We will later
			// assign the default material to all faces using *this*
			// material. Use 0xcdcdcdcd as special value to indicate
			// this.
			iIndex = 0xcdcdcdcd;
		}
		this->mCurrent = sz;
		iCnt = (int)(*((uint16_t*)this->mCurrent));
		this->mCurrent += sizeof(uint16_t);

		for (unsigned int i = 0; i < iCnt;++i)
		{
			iTemp = (uint16_t)*((uint16_t*)this->mCurrent);

			// check range
			if (iTemp >= mMesh.mFaceMaterials.size())
			{
				DefaultLogger::get()->error("Invalid face index in face material list");
				mMesh.mFaceMaterials[mMesh.mFaceMaterials.size()-1] = iIndex;
			}
			else
			{
				mMesh.mFaceMaterials[iTemp] = iIndex;
			}
			this->mCurrent += sizeof(uint16_t);
		}

		break;
	};
	ASSIMP_3DS_VALIDATE_CHUNK_SIZE();
	ASSIMP_3DS_END_CHUNK();
	return ParseFaceChunk(piRemaining);
}
// ------------------------------------------------------------------------------------------------
void Dot3DSImporter::ParseMeshChunk(int& piRemaining)
{
	ASSIMP_3DS_BEGIN_CHUNK();
	Dot3DS::Mesh& mMesh = this->mScene->mMeshes.back();

	// get chunk type
	const unsigned char* sz = this->mCurrent;
	unsigned int iCnt = 0;
	int iRemaining;
	uint16_t iNum = 0;
	float* pf;
	switch (psChunk->Flag)
	{
	case Dot3DSFile::CHUNK_VERTLIST:

		iNum = *((short*)this->mCurrent);
		this->mCurrent += sizeof(short);
		while (iNum-- > 0)
		{
			mMesh.mPositions.push_back(*((aiVector3D*)this->mCurrent));
			aiVector3D& v = mMesh.mPositions.back();
			std::swap( v.y, v.z);
			//v.y *= -1.0f;
			this->mCurrent += sizeof(aiVector3D);
		}
		break;
	case Dot3DSFile::CHUNK_TRMATRIX:
		{
		pf = (float*)this->mCurrent;
		this->mCurrent += 12 * sizeof(float);

		mMesh.mMat.a1 = pf[0];
		mMesh.mMat.b1 = pf[1];
		mMesh.mMat.c1 = pf[2];
		mMesh.mMat.a2 = pf[3];
		mMesh.mMat.b2 = pf[4];
		mMesh.mMat.c2 = pf[5];
		mMesh.mMat.a3 = pf[6];
		mMesh.mMat.b3 = pf[7];
		mMesh.mMat.c3 = pf[8];
		mMesh.mMat.a4 = pf[9];
		mMesh.mMat.b4 = pf[10];
		mMesh.mMat.c4 = pf[11];

		// now check whether the matrix has got a negative determinant
		// If yes, we need to flip all vertices' x axis ....
		// From lib3ds, mesh.c
		if (mMesh.mMat.Determinant() < 0.0f)
			{
			aiMatrix4x4 mInv = mMesh.mMat;
			mInv.Inverse();

			aiMatrix4x4 mMe = mMesh.mMat;
			mMe.a1 *= -1.0f;
			mMe.b1 *= -1.0f;
			mMe.c1 *= -1.0f;
			mMe.d1 *= -1.0f;
			mInv = mInv * mMe;
			for (register unsigned int i = 0; i < mMesh.mPositions.size();++i)
				{
				aiVector3D a,c;
				a = mMesh.mPositions[i];
				c[0]= mInv[0][0]*a[0] + mInv[1][0]*a[1] + mInv[2][0]*a[2] + mInv[3][0];
				c[1]= mInv[0][1]*a[0] + mInv[1][1]*a[1] + mInv[2][1]*a[2] + mInv[3][1];
				c[2]= mInv[0][2]*a[0] + mInv[1][2]*a[1] + mInv[2][2]*a[2] + mInv[3][2];
				mMesh.mPositions[i] = c;
				}
			}
			
		}
		break;
	case Dot3DSFile::CHUNK_MAPLIST:

		iNum = *((uint16_t*)this->mCurrent);
		this->mCurrent += sizeof(uint16_t);
		while (iNum-- > 0)
		{
			mMesh.mTexCoords.push_back(*((aiVector2D*)this->mCurrent));
			this->mCurrent += sizeof(aiVector2D);
		}
		break;

	case Dot3DSFile::CHUNK_FACELIST:

		iNum = *((uint16_t*)this->mCurrent);
		this->mCurrent += sizeof(uint16_t);
		while (iNum-- > 0)
		{
			Dot3DS::Face sFace;
			sFace.mIndices[0] = *((uint16_t*)this->mCurrent);
			this->mCurrent += sizeof(uint16_t);
			sFace.mIndices[1] = *((uint16_t*)this->mCurrent);
			this->mCurrent += sizeof(uint16_t);
			sFace.mIndices[2] = *((uint16_t*)this->mCurrent);
			this->mCurrent += 2*sizeof(uint16_t);
			mMesh.mFaces.push_back(sFace);
		}

		// resize the material array (0xcdcdcdcd marks the
		// default material; so if a face is not referenced
		// by a material $$DEFAULT will be assigned to it)
		mMesh.mFaceMaterials.resize(mMesh.mFaces.size(),0xcdcdcdcd);

		iRemaining = (int)(pcCurNext - this->mCurrent);
		if (iRemaining > 0)this->ParseFaceChunk(iRemaining);
		break;

	};
	ASSIMP_3DS_VALIDATE_CHUNK_SIZE();
	ASSIMP_3DS_END_CHUNK();
	return ParseMeshChunk(piRemaining);
}
// ------------------------------------------------------------------------------------------------
void Dot3DSImporter::ParseMaterialChunk(int& piRemaining)
{
	ASSIMP_3DS_BEGIN_CHUNK();

	// get chunk type
	const unsigned char* sz = this->mCurrent;
	unsigned int iCnt = 0;
	int iRemaining;
	aiColor3D* pc;
	float* pcf;
	switch (psChunk->Flag)
	{
	case Dot3DSFile::CHUNK_MAT_MATNAME:

		// string in file is zero-terminated, 
		// this should be no problem. However, validate whether it overlaps 
		// the end of the chunk, if yes we should truncate it.
		while (*sz++ != '\0')
		{
			if (sz > pcCurNext-1)
			{
				DefaultLogger::get()->error("Material name string is too long");
				break;
			}
			++iCnt;
		}
		this->mScene->mMaterials.back().mName = std::string((const char*)this->mCurrent,iCnt);
		break;
	case Dot3DSFile::CHUNK_MAT_DIFFUSE:
		pc = &this->mScene->mMaterials.back().mDiffuse;
		this->ParseColorChunk(pc);
		if (is_qnan(pc->r))
		{
			// color chunk is invalid. Simply ignore it
			DefaultLogger::get()->error("Unable to read DIFFUSE chunk");
			pc->r = pc->g = pc->b = 1.0f;
		}
		break;
	case Dot3DSFile::CHUNK_MAT_SPECULAR:
		pc = &this->mScene->mMaterials.back().mSpecular;
		this->ParseColorChunk(pc);
		if (is_qnan(pc->r))
		{
			// color chunk is invalid. Simply ignore it
			DefaultLogger::get()->error("Unable to read SPECULAR chunk");
			pc->r = pc->g = pc->b = 1.0f;
		}
		break;
	case Dot3DSFile::CHUNK_MAT_AMBIENT:
		pc = &this->mScene->mMaterials.back().mAmbient;
		this->ParseColorChunk(pc);
		if (is_qnan(pc->r))
		{
			// color chunk is invalid. Simply ignore it
			DefaultLogger::get()->error("Unable to read AMBIENT chunk");
			pc->r = pc->g = pc->b = 1.0f;
		}
		break;
	case Dot3DSFile::CHUNK_MAT_SELF_ILLUM:
		pc = &this->mScene->mMaterials.back().mEmissive;
		this->ParseColorChunk(pc);
		if (is_qnan(pc->r))
		{
			// color chunk is invalid. Simply ignore it
			// EMISSSIVE TO 0|0|0
			DefaultLogger::get()->error("Unable to read EMISSIVE chunk");
			pc->r = pc->g = pc->b = 0.0f;
		}
		break;
	case Dot3DSFile::CHUNK_MAT_TRANSPARENCY:
		pcf = &this->mScene->mMaterials.back().mTransparency;
		*pcf = this->ParsePercentageChunk();
		// NOTE: transparency, not opacity
		if (is_qnan(*pcf))*pcf = 1.0f;
		else *pcf = 1.0f - *pcf * (float)0xFFFF / 100.0f;
		break;

	case Dot3DSFile::CHUNK_MAT_SHADING:
		
		this->mScene->mMaterials.back().mShading =
			(Dot3DS::Dot3DSFile::shadetype3ds)*((uint16_t*)this->mCurrent);

		this->mCurrent += sizeof(uint16_t);
		break;

	case Dot3DSFile::CHUNK_MAT_TWO_SIDE:
		this->mScene->mMaterials.back().mTwoSided = true;
		break;

	case Dot3DSFile::CHUNK_MAT_SHININESS:
		pcf = &this->mScene->mMaterials.back().mSpecularExponent;
		*pcf = this->ParsePercentageChunk();
		if (is_qnan(*pcf))*pcf = 0.0f;
		else *pcf *= (float)0xFFFF;
		break;

	case Dot3DSFile::CHUNK_MAT_SHININESS_PERCENT:
		pcf = &this->mScene->mMaterials.back().mShininessStrength;
		*pcf = this->ParsePercentageChunk();
		if (is_qnan(*pcf))*pcf = 0.0f;
		else *pcf *= (float)0xffff / 100.0f;
		break;

	case Dot3DSFile::CHUNK_MAT_SELF_ILPCT:
		// TODO: need to multiply with emissive base color?
		pcf = &this->mScene->mMaterials.back().sTexEmissive.mTextureBlend;
		*pcf = this->ParsePercentageChunk();
		if (is_qnan(*pcf))*pcf = 0.0f;
		else *pcf = *pcf * (float)0xFFFF / 100.0f;
		break;

	// parse texture chunks
	case Dot3DSFile::CHUNK_MAT_TEXTURE:
		iRemaining = (psChunk->Size - sizeof(Dot3DSFile::Chunk));
		this->ParseTextureChunk(iRemaining,&this->mScene->mMaterials.back().sTexDiffuse);
		break;
	case Dot3DSFile::CHUNK_MAT_BUMPMAP:
		iRemaining = (psChunk->Size - sizeof(Dot3DSFile::Chunk));
		this->ParseTextureChunk(iRemaining,&this->mScene->mMaterials.back().sTexBump);
		break;
	case Dot3DSFile::CHUNK_MAT_OPACMAP:
		iRemaining = (psChunk->Size - sizeof(Dot3DSFile::Chunk));
		this->ParseTextureChunk(iRemaining,&this->mScene->mMaterials.back().sTexOpacity);
		break;
	case Dot3DSFile::CHUNK_MAT_MAT_SHINMAP:
		iRemaining = (psChunk->Size - sizeof(Dot3DSFile::Chunk));
		this->ParseTextureChunk(iRemaining,&this->mScene->mMaterials.back().sTexShininess);
		break;
	case Dot3DSFile::CHUNK_MAT_SPECMAP:
		iRemaining = (psChunk->Size - sizeof(Dot3DSFile::Chunk));
		this->ParseTextureChunk(iRemaining,&this->mScene->mMaterials.back().sTexSpecular);
		break;
	case Dot3DSFile::CHUNK_MAT_SELFIMAP:
		iRemaining = (psChunk->Size - sizeof(Dot3DSFile::Chunk));
		this->ParseTextureChunk(iRemaining,&this->mScene->mMaterials.back().sTexEmissive);
		break;
	};
	ASSIMP_3DS_VALIDATE_CHUNK_SIZE();
	ASSIMP_3DS_END_CHUNK();
	return ParseMaterialChunk(piRemaining);
}
// ------------------------------------------------------------------------------------------------
void Dot3DSImporter::ParseTextureChunk(int& piRemaining,Dot3DS::Texture* pcOut)
{
	ASSIMP_3DS_BEGIN_CHUNK();

	// get chunk type
	const unsigned char* sz = this->mCurrent;
	unsigned int iCnt = 0;
	switch (psChunk->Flag)
	{
	case Dot3DSFile::CHUNK_MAPFILE:
		// string in file is zero-terminated, 
		// this should be no problem. However, validate whether
		// it overlaps the end of the chunk, if yes we should
		// truncate it.
		while (*sz++ != '\0')
		{
			if (sz > pcCurNext-1)break;
			++iCnt;
		}
		pcOut->mMapName = std::string((const char*)this->mCurrent,iCnt);
		break;
	// manually parse the blend factor
	case Dot3DSFile::CHUNK_PERCENTF:
		pcOut->mTextureBlend = *((float*)this->mCurrent);
		break;
	// manually parse the blend factor
	case Dot3DSFile::CHUNK_PERCENTW:
		pcOut->mTextureBlend = (float)(*((short*)this->mCurrent)) / 100.0f;
		break;

	case Dot3DSFile::CHUNK_MAT_MAP_USCALE:
		pcOut->mScaleU = *((float*)this->mCurrent);
		if (0.0f == pcOut->mScaleU)
		{
			DefaultLogger::get()->warn("Texture coordinate scaling in the "
				"x direction is zero. Assuming this should be 1.0 ... ");
			pcOut->mScaleU = 1.0f;
		}
		// NOTE: some docs state it is 1/u, others say it is u ... ARGHH!
		//pcOut->mScaleU = 1.0f / pcOut->mScaleU;
		break;
	case Dot3DSFile::CHUNK_MAT_MAP_VSCALE:
		pcOut->mScaleV = *((float*)this->mCurrent);
		if (0.0f == pcOut->mScaleV)
		{
			DefaultLogger::get()->warn("Texture coordinate scaling in the "
				"y direction is zero. Assuming this should be 1.0 ... ");
			pcOut->mScaleV = 1.0f;
		}
		// NOTE: some docs state it is 1/v, others say it is v ... ARGHH!
		//pcOut->mScaleV = 1.0f / pcOut->mScaleV;
		break;
	case Dot3DSFile::CHUNK_MAT_MAP_UOFFSET:
		pcOut->mOffsetU = *((float*)this->mCurrent);
		break;
	case Dot3DSFile::CHUNK_MAT_MAP_VOFFSET:
		pcOut->mOffsetV = *((float*)this->mCurrent);
		break;
	case Dot3DSFile::CHUNK_MAT_MAP_ANG:
		pcOut->mRotation = *((float*)this->mCurrent);
		break;
	case Dot3DSFile::CHUNK_MAT_MAP_TILING:
		uint16_t iFlags = *((uint16_t*)this->mCurrent);

		// check whether the mirror flag is set
		if (iFlags & 0x2u)
		{
			pcOut->mMapMode = aiTextureMapMode_Mirror;
		}
		// assume that "decal" means clamping ...
		else if (iFlags & 0x10u && iFlags & 0x1u)
		{
			pcOut->mMapMode = aiTextureMapMode_Clamp;
		}
		break;
	};

	ASSIMP_3DS_VALIDATE_CHUNK_SIZE();
	ASSIMP_3DS_END_CHUNK();
	return ParseTextureChunk(piRemaining,pcOut);
}
// ------------------------------------------------------------------------------------------------
float Dot3DSImporter::ParsePercentageChunk()
{
	const Dot3DSFile::Chunk* psChunk;
	this->ReadChunk(&psChunk);
	if (NULL == psChunk)return std::numeric_limits<float>::quiet_NaN();

	if (Dot3DSFile::CHUNK_PERCENTF == psChunk->Flag)
	{
		if (sizeof(float) > psChunk->Size)
			return std::numeric_limits<float>::quiet_NaN();
		return *((float*)this->mCurrent);
	}
	else if (Dot3DSFile::CHUNK_PERCENTW == psChunk->Flag)
	{
		if (2 > psChunk->Size)
			return std::numeric_limits<float>::quiet_NaN();
		return (float)(*((short*)this->mCurrent)) / (float)0xFFFF;
	}
	this->mCurrent += psChunk->Size - sizeof(Dot3DSFile::Chunk);
	return std::numeric_limits<float>::quiet_NaN();
}
// ------------------------------------------------------------------------------------------------
void Dot3DSImporter::ParseColorChunk(aiColor3D* p_pcOut,
	bool p_bAcceptPercent)
{
	ai_assert(p_pcOut != NULL);

	// error return value
	static const aiColor3D clrError = aiColor3D(std::numeric_limits<float>::quiet_NaN(),
		std::numeric_limits<float>::quiet_NaN(),
		std::numeric_limits<float>::quiet_NaN());

	const Dot3DSFile::Chunk* psChunk;
	this->ReadChunk(&psChunk);
	if (!psChunk)
	{
		*p_pcOut = clrError;
		return;
	}
	const unsigned int diff = psChunk->Size - sizeof(Dot3DSFile::Chunk);

	const unsigned char* pcCur = this->mCurrent;
	this->mCurrent += diff;
	bool bGamma = false;
	switch(psChunk->Flag)
	{
	case Dot3DSFile::CHUNK_LINRGBF:
		bGamma = true;
	case Dot3DSFile::CHUNK_RGBF:
		if (sizeof(float) * 3 > diff)
		{
			*p_pcOut = clrError;
			return;
		}
		p_pcOut->r = ((float*)pcCur)[0];
		p_pcOut->g = ((float*)pcCur)[1];
		p_pcOut->b = ((float*)pcCur)[2];
		break;

	case Dot3DSFile::CHUNK_LINRGBB:
		bGamma = true;
	case Dot3DSFile::CHUNK_RGBB:
		if (sizeof(char) * 3 > diff)
		{
			*p_pcOut = clrError;
			return;
		}
		p_pcOut->r = (float)pcCur[0] / 255.0f;
		p_pcOut->g = (float)pcCur[1] / 255.0f;
		p_pcOut->b = (float)pcCur[2] / 255.0f;
		break;

	// percentage chunks: accepted to be compatible with various
	// .3ds files with very curious content
	case Dot3DSFile::CHUNK_PERCENTF:
		if (p_bAcceptPercent && 4 <= diff)
		{
			p_pcOut->r = *((float*)pcCur);
			p_pcOut->g = *((float*)pcCur);
			p_pcOut->b = *((float*)pcCur);
			break;
		}
		*p_pcOut = clrError;
		return;
	case Dot3DSFile::CHUNK_PERCENTW:
		if (p_bAcceptPercent && 1 <= diff)
		{
			p_pcOut->r = (float)pcCur[0] / 255.0f;
			p_pcOut->g = (float)pcCur[0] / 255.0f;
			p_pcOut->b = (float)pcCur[0] / 255.0f;
			break;
		}
		*p_pcOut = clrError;
		return;

	default:
		// skip unknown chunks, hope this won't cause any problems.
		return this->ParseColorChunk(p_pcOut,p_bAcceptPercent);
	};
	if (bGamma)
	{
		p_pcOut->r = powf(p_pcOut->r, 1.0f / 2.2f);
		p_pcOut->g = powf(p_pcOut->g, 1.0f / 2.2f);
		p_pcOut->b = powf(p_pcOut->b, 1.0f / 2.2f);
	}
	return;
}
