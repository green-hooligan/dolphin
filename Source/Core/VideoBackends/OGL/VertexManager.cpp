// Copyright 2013 Dolphin Emulator Project
// Licensed under GPLv2
// Refer to the license.txt file included.

#include "Globals.h"

#include <fstream>
#include <vector>

#include "Fifo.h"

#include "DriverDetails.h"
#include "VideoConfig.h"
#include "Statistics.h"
#include "MemoryUtil.h"
#include "Render.h"
#include "ImageWrite.h"
#include "BPMemory.h"
#include "TextureCache.h"
#include "PixelShaderManager.h"
#include "VertexShaderManager.h"
#include "ProgramShaderCache.h"
#include "VertexShaderGen.h"
#include "VertexLoader.h"
#include "VertexManager.h"
#include "IndexGenerator.h"
#include "FileUtil.h"
#include "StreamBuffer.h"
#include "Render.h"

#include "main.h"

// internal state for loading vertices
extern NativeVertexFormat *g_nativeVertexFmt;

namespace OGL
{
//This are the initially requested size for the buffers expressed in bytes
const u32 MAX_IBUFFER_SIZE =  2*1024*1024;
const u32 MAX_VBUFFER_SIZE = 32*1024*1024;

static StreamBuffer *s_vertexBuffer;
static StreamBuffer *s_indexBuffer;
static size_t s_baseVertex;
static size_t s_index_offset;

VertexManager::VertexManager()
{
	CreateDeviceObjects();
}

VertexManager::~VertexManager()
{
	DestroyDeviceObjects();
}

void VertexManager::CreateDeviceObjects()
{
	s_vertexBuffer = StreamBuffer::Create(GL_ARRAY_BUFFER, MAX_VBUFFER_SIZE);
	m_vertex_buffers = s_vertexBuffer->m_buffer;

	s_indexBuffer = StreamBuffer::Create(GL_ELEMENT_ARRAY_BUFFER, MAX_IBUFFER_SIZE);
	m_index_buffers = s_indexBuffer->m_buffer;

	m_CurrentVertexFmt = NULL;
	m_last_vao = 0;
}

void VertexManager::DestroyDeviceObjects()
{
	GL_REPORT_ERRORD();
	glBindBuffer(GL_ARRAY_BUFFER, 0 );
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0 );
	GL_REPORT_ERROR();

	delete s_vertexBuffer;
	delete s_indexBuffer;
	GL_REPORT_ERROR();
}

void VertexManager::PrepareDrawBuffers(u32 stride)
{
	u32 vertex_data_size = IndexGenerator::GetNumVerts() * stride;
	u32 index_data_size = IndexGenerator::GetIndexLen() * sizeof(u16);

	s_vertexBuffer->Unmap(vertex_data_size);
	s_indexBuffer->Unmap(index_data_size);

	ADDSTAT(stats.thisFrame.bytesVertexStreamed, vertex_data_size);
	ADDSTAT(stats.thisFrame.bytesIndexStreamed, index_data_size);
}

void VertexManager::ResetBuffer(u32 stride)
{
	auto buffer = s_vertexBuffer->Map(MAXVBUFFERSIZE, stride);
	s_pCurBufferPointer = s_pBaseBufferPointer = buffer.first;
	s_pEndBufferPointer = buffer.first + MAXVBUFFERSIZE;
	s_baseVertex = buffer.second / stride;

	buffer = s_indexBuffer->Map(MAXIBUFFERSIZE * sizeof(u16));
	IndexGenerator::Start((u16*)buffer.first);
	s_index_offset = buffer.second;
}

void VertexManager::Draw(u32 stride)
{
	u32 index_size = IndexGenerator::GetIndexLen();
	u32 max_index = IndexGenerator::GetNumVerts();
	GLenum primitive_mode = 0;

	switch(current_primitive_type)
	{
		case PRIMITIVE_POINTS:
			primitive_mode = GL_POINTS;
			break;
		case PRIMITIVE_LINES:
			primitive_mode = GL_LINES;
			break;
		case PRIMITIVE_TRIANGLES:
			primitive_mode = g_ActiveConfig.backend_info.bSupportsPrimitiveRestart ? GL_TRIANGLE_STRIP : GL_TRIANGLES;
			break;
	}

	if(g_ogl_config.bSupportsGLBaseVertex) {
		glDrawRangeElementsBaseVertex(primitive_mode, 0, max_index, index_size, GL_UNSIGNED_SHORT, (u8*)NULL+s_index_offset, (GLint)s_baseVertex);
	} else {
		glDrawRangeElements(primitive_mode, 0, max_index, index_size, GL_UNSIGNED_SHORT, (u8*)NULL+s_index_offset);
	}
	INCSTAT(stats.thisFrame.numIndexedDrawCalls);
}

void VertexManager::vFlush(bool useDstAlpha)
{
	GLVertexFormat *nativeVertexFmt = (GLVertexFormat*)g_nativeVertexFmt;
	u32 stride  = nativeVertexFmt->GetVertexStride();

	if(m_last_vao != nativeVertexFmt->VAO) {
		glBindVertexArray(nativeVertexFmt->VAO);
		m_last_vao = nativeVertexFmt->VAO;
	}

	PrepareDrawBuffers(stride);
	GL_REPORT_ERRORD();

	// Makes sure we can actually do Dual source blending
	bool dualSourcePossible = g_ActiveConfig.backend_info.bSupportsDualSourceBlend;

	// finally bind
	if (dualSourcePossible)
	{
		if (useDstAlpha)
		{
			// If host supports GL_ARB_blend_func_extended, we can do dst alpha in
			// the same pass as regular rendering.
			ProgramShaderCache::SetShader(DSTALPHA_DUAL_SOURCE_BLEND, g_nativeVertexFmt->m_components);
		}
		else
		{
			ProgramShaderCache::SetShader(DSTALPHA_NONE,g_nativeVertexFmt->m_components);
		}
	}
	else
	{
		ProgramShaderCache::SetShader(DSTALPHA_NONE,g_nativeVertexFmt->m_components);
	}

	// upload global constants
	ProgramShaderCache::UploadConstants();

	// setup the pointers
	if (g_nativeVertexFmt)
		g_nativeVertexFmt->SetupVertexPointers();
	GL_REPORT_ERRORD();

	Draw(stride);

	// run through vertex groups again to set alpha
	if (useDstAlpha && !dualSourcePossible)
	{
		ProgramShaderCache::SetShader(DSTALPHA_ALPHA_PASS,g_nativeVertexFmt->m_components);

		// only update alpha
		glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);

		glDisable(GL_BLEND);

		Draw(stride);

		// restore color mask
		g_renderer->SetColorMask();

		if (bpmem.blendmode.blendenable || bpmem.blendmode.subtract)
			glEnable(GL_BLEND);
	}

#if defined(_DEBUG) || defined(DEBUGFAST)
	if (g_ActiveConfig.iLog & CONF_SAVESHADERS)
	{
		// save the shaders
		ProgramShaderCache::PCacheEntry prog = ProgramShaderCache::GetShaderProgram();
		char strfile[255];
		sprintf(strfile, "%sps%.3d.txt", File::GetUserPath(D_DUMPFRAMES_IDX).c_str(), g_ActiveConfig.iSaveTargetId);
		std::ofstream fps;
		OpenFStream(fps, strfile, std::ios_base::out);
		fps << prog.shader.strpprog.c_str();
		sprintf(strfile, "%svs%.3d.txt", File::GetUserPath(D_DUMPFRAMES_IDX).c_str(), g_ActiveConfig.iSaveTargetId);
		std::ofstream fvs;
		OpenFStream(fvs, strfile, std::ios_base::out);
		fvs << prog.shader.strvprog.c_str();
	}

	if (g_ActiveConfig.iLog & CONF_SAVETARGETS)
	{
		char str[128];
		sprintf(str, "%starg%.3d.png", File::GetUserPath(D_DUMPFRAMES_IDX).c_str(), g_ActiveConfig.iSaveTargetId);
		TargetRectangle tr;
		tr.left = 0;
		tr.right = Renderer::GetTargetWidth();
		tr.top = 0;
		tr.bottom = Renderer::GetTargetHeight();
		g_renderer->SaveScreenshot(str, tr);
	}
#endif
	g_Config.iSaveTargetId++;

	ClearEFBCache();

	GL_REPORT_ERRORD();
}


}  // namespace
