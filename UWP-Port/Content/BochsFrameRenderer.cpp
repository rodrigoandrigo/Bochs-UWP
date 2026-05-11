#include "pch.h"
#include "BochsFrameRenderer.h"

#include "..\Common\DirectXHelper.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

using namespace UWP_Port;
using namespace Microsoft::WRL;
using namespace DirectX;
using namespace Concurrency;

namespace
{
	struct FrameVertex
	{
		XMFLOAT2 position;
		XMFLOAT2 texcoord;
	};
}

BochsFrameRenderer::BochsFrameRenderer(const std::shared_ptr<DX::DeviceResources>& deviceResources) :
	m_deviceResources(deviceResources),
	m_textureWidth(0),
	m_textureHeight(0),
	m_sourceWidth(0),
	m_sourceHeight(0),
	m_loadingComplete(false)
{
	CreateDeviceDependentResources();
}

void BochsFrameRenderer::CreateWindowSizeDependentResources()
{
}

void BochsFrameRenderer::CreateDeviceDependentResources()
{
	auto loadVSTask = DX::ReadDataAsync(L"BochsFrameVertexShader.cso");
	auto loadPSTask = DX::ReadDataAsync(L"BochsFramePixelShader.cso");

	auto createVSTask = loadVSTask.then([this](const std::vector<byte>& fileData)
	{
		DX::ThrowIfFailed(
			m_deviceResources->GetD3DDevice()->CreateVertexShader(
				fileData.data(),
				fileData.size(),
				nullptr,
				&m_vertexShader
			)
		);

		static const D3D11_INPUT_ELEMENT_DESC vertexDesc[] =
		{
			{ "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		};

		DX::ThrowIfFailed(
			m_deviceResources->GetD3DDevice()->CreateInputLayout(
				vertexDesc,
				ARRAYSIZE(vertexDesc),
				fileData.data(),
				fileData.size(),
				&m_inputLayout
			)
		);
	});

	auto createPSTask = loadPSTask.then([this](const std::vector<byte>& fileData)
	{
		DX::ThrowIfFailed(
			m_deviceResources->GetD3DDevice()->CreatePixelShader(
				fileData.data(),
				fileData.size(),
				nullptr,
				&m_pixelShader
			)
		);
	});

	(createVSTask && createPSTask).then([this]()
	{
		D3D11_SAMPLER_DESC samplerDesc = {};
		samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
		samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
		samplerDesc.MinLOD = 0;
		samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
		DX::ThrowIfFailed(
			m_deviceResources->GetD3DDevice()->CreateSamplerState(
				&samplerDesc,
				&m_samplerState
			)
		);

		CD3D11_BUFFER_DESC constantBufferDesc(sizeof(BochsFrameConstants), D3D11_BIND_CONSTANT_BUFFER);
		DX::ThrowIfFailed(
			m_deviceResources->GetD3DDevice()->CreateBuffer(
				&constantBufferDesc,
				nullptr,
				&m_constantBuffer
			)
		);

		static const FrameVertex vertices[] =
		{
			{ XMFLOAT2(-1.0f, -1.0f), XMFLOAT2(0.0f, 1.0f) },
			{ XMFLOAT2(-1.0f,  1.0f), XMFLOAT2(0.0f, 0.0f) },
			{ XMFLOAT2( 1.0f, -1.0f), XMFLOAT2(1.0f, 1.0f) },
			{ XMFLOAT2( 1.0f,  1.0f), XMFLOAT2(1.0f, 0.0f) },
		};
		D3D11_SUBRESOURCE_DATA vertexData = {};
		vertexData.pSysMem = vertices;
		CD3D11_BUFFER_DESC vertexBufferDesc(sizeof(vertices), D3D11_BIND_VERTEX_BUFFER);
		DX::ThrowIfFailed(
			m_deviceResources->GetD3DDevice()->CreateBuffer(
				&vertexBufferDesc,
				&vertexData,
				&m_vertexBuffer
			)
		);

		m_loadingComplete = true;
	});
}

void BochsFrameRenderer::ReleaseDeviceDependentResources()
{
	m_loadingComplete = false;
	m_vertexShader.Reset();
	m_pixelShader.Reset();
	m_inputLayout.Reset();
	m_vertexBuffer.Reset();
	m_frameTexture.Reset();
	m_frameTextureView.Reset();
	m_samplerState.Reset();
	m_constantBuffer.Reset();
	m_textureWidth = 0;
	m_textureHeight = 0;
	m_sourceWidth = 0;
	m_sourceHeight = 0;
	m_scaledPixels.clear();
}

unsigned BochsFrameRenderer::MaxTextureDimension() const
{
	D3D_FEATURE_LEVEL level = m_deviceResources->GetD3DDevice()->GetFeatureLevel();
	if (level >= D3D_FEATURE_LEVEL_10_0)
	{
		return D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION;
	}
	if (level >= D3D_FEATURE_LEVEL_9_3)
	{
		return 4096;
	}
	return 2048;
}

Windows::Foundation::Size BochsFrameRenderer::UploadTextureSize(const BochsFrameSnapshot& frame) const
{
	unsigned maxDimension = MaxTextureDimension();
	if (frame.width <= maxDimension && frame.height <= maxDimension)
	{
		return Windows::Foundation::Size(
			static_cast<float>(frame.width),
			static_cast<float>(frame.height));
	}

	float scale = (std::min)(
		static_cast<float>(maxDimension) / static_cast<float>(frame.width),
		static_cast<float>(maxDimension) / static_cast<float>(frame.height));
	unsigned width = (std::max)(1u, static_cast<unsigned>(static_cast<float>(frame.width) * scale));
	unsigned height = (std::max)(1u, static_cast<unsigned>(static_cast<float>(frame.height) * scale));
	return Windows::Foundation::Size(static_cast<float>(width), static_cast<float>(height));
}

bool BochsFrameRenderer::UsesScaledUpload(const BochsFrameSnapshot& frame) const
{
	Windows::Foundation::Size uploadSize = UploadTextureSize(frame);
	return static_cast<unsigned>(uploadSize.Width) != frame.width ||
		static_cast<unsigned>(uploadSize.Height) != frame.height;
}

const std::vector<uint32_t>& BochsFrameRenderer::BuildScaledPixels(const BochsFrameSnapshot& frame)
{
	Windows::Foundation::Size uploadSize = UploadTextureSize(frame);
	unsigned width = static_cast<unsigned>(uploadSize.Width);
	unsigned height = static_cast<unsigned>(uploadSize.Height);
	m_scaledPixels.resize(static_cast<size_t>(width) * static_cast<size_t>(height));

	for (unsigned y = 0; y < height; y++)
	{
		unsigned sourceY = static_cast<unsigned>(
			(static_cast<unsigned long long>(y) * frame.height) / height);
		const uint32_t* sourceRow = frame.pixels.data() + static_cast<size_t>(sourceY) * frame.width;
		uint32_t* targetRow = m_scaledPixels.data() + static_cast<size_t>(y) * width;
		for (unsigned x = 0; x < width; x++)
		{
			unsigned sourceX = static_cast<unsigned>(
				(static_cast<unsigned long long>(x) * frame.width) / width);
			targetRow[x] = sourceRow[sourceX];
		}
	}

	return m_scaledPixels;
}

void BochsFrameRenderer::EnsureFrameTexture(const BochsFrameSnapshot& frame)
{
	Windows::Foundation::Size uploadSize = UploadTextureSize(frame);
	unsigned textureWidth = static_cast<unsigned>(uploadSize.Width);
	unsigned textureHeight = static_cast<unsigned>(uploadSize.Height);

	if (m_frameTexture &&
		m_textureWidth == textureWidth &&
		m_textureHeight == textureHeight &&
		m_sourceWidth == frame.width &&
		m_sourceHeight == frame.height)
	{
		return;
	}

	m_frameTexture.Reset();
	m_frameTextureView.Reset();

	D3D11_TEXTURE2D_DESC textureDesc = {};
	textureDesc.Width = textureWidth;
	textureDesc.Height = textureHeight;
	textureDesc.MipLevels = 1;
	textureDesc.ArraySize = 1;
	textureDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	textureDesc.SampleDesc.Count = 1;
	textureDesc.SampleDesc.Quality = 0;
	textureDesc.Usage = D3D11_USAGE_DEFAULT;
	textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	textureDesc.CPUAccessFlags = 0;

	DX::ThrowIfFailed(
		m_deviceResources->GetD3DDevice()->CreateTexture2D(
			&textureDesc,
			nullptr,
			&m_frameTexture
		)
	);

	D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc = {};
	viewDesc.Format = textureDesc.Format;
	viewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	viewDesc.Texture2D.MipLevels = 1;
	DX::ThrowIfFailed(
		m_deviceResources->GetD3DDevice()->CreateShaderResourceView(
			m_frameTexture.Get(),
			&viewDesc,
			&m_frameTextureView
		)
	);

	m_textureWidth = textureWidth;
	m_textureHeight = textureHeight;
	m_sourceWidth = frame.width;
	m_sourceHeight = frame.height;
}

void BochsFrameRenderer::UploadFrameTexture(const BochsFrameSnapshot& frame)
{
	if (frame.pixels.empty())
	{
		return;
	}

	auto context = m_deviceResources->GetD3DDeviceContext();
	if (UsesScaledUpload(frame))
	{
		const std::vector<uint32_t>& scaledPixels = BuildScaledPixels(frame);
		context->UpdateSubresource1(
			m_frameTexture.Get(),
			0,
			nullptr,
			scaledPixels.data(),
			m_textureWidth * 4,
			0,
			0);
		return;
	}

	const void* source = frame.pixels.data();
	const D3D11_BOX* updateBox = nullptr;
	D3D11_BOX dirtyBox = {};
	if (frame.dirtyRect.valid &&
		frame.dirtyRect.width > 0 &&
		frame.dirtyRect.height > 0 &&
		(frame.dirtyRect.width != frame.width || frame.dirtyRect.height != frame.height))
	{
		dirtyBox.left = frame.dirtyRect.x;
		dirtyBox.top = frame.dirtyRect.y;
		dirtyBox.front = 0;
		dirtyBox.right = frame.dirtyRect.x + frame.dirtyRect.width;
		dirtyBox.bottom = frame.dirtyRect.y + frame.dirtyRect.height;
		dirtyBox.back = 1;
		updateBox = &dirtyBox;
		source = frame.pixels.data() + static_cast<size_t>(frame.dirtyRect.y) * frame.width + frame.dirtyRect.x;
	}

	context->UpdateSubresource1(
		m_frameTexture.Get(),
		0,
		updateBox,
		source,
		frame.width * 4,
		0,
		0);
}

BochsFrameConstants BochsFrameRenderer::BuildConstants(const BochsFrameSnapshot& frame) const
{
	Windows::Foundation::Size output = m_deviceResources->GetOutputSize();
	float outputWidth = output.Width > 1.0f ? output.Width : 1.0f;
	float outputHeight = output.Height > 1.0f ? output.Height : 1.0f;
	float frameAspect = static_cast<float>(frame.width) / static_cast<float>(frame.height);
	float outputAspect = outputWidth / outputHeight;

	float scaleX = 1.0f;
	float scaleY = 1.0f;
	if (outputAspect > frameAspect)
	{
		scaleX = frameAspect / outputAspect;
	}
	else
	{
		scaleY = outputAspect / frameAspect;
	}

	BochsFrameConstants constants = {};
	constants.scaleOffset = XMFLOAT4(scaleX, scaleY, 0.0f, 0.0f);
	return constants;
}

bool BochsFrameRenderer::Render()
{
	auto context = m_deviceResources->GetD3DDeviceContext();
	auto viewport = m_deviceResources->GetScreenViewport();
	context->RSSetViewports(1, &viewport);

	ID3D11RenderTargetView* const targets[1] = { m_deviceResources->GetBackBufferRenderTargetView() };
	context->OMSetRenderTargets(1, targets, nullptr);
	context->ClearRenderTargetView(m_deviceResources->GetBackBufferRenderTargetView(), DirectX::Colors::Black);

	if (!m_loadingComplete)
	{
		return true;
	}

	BochsFrameSnapshot frame = BochsUwpBridge::CopyFrame(m_frameTexture == nullptr);
	if (!frame.valid)
	{
		return true;
	}
	if (frame.pixels.empty() &&
		(m_frameTexture == nullptr || m_sourceWidth != frame.width || m_sourceHeight != frame.height))
	{
		frame = BochsUwpBridge::CopyFrame(true);
	}

	EnsureFrameTexture(frame);
	if (frame.dirty)
	{
		UploadFrameTexture(frame);
	}

	BochsFrameConstants constants = BuildConstants(frame);
	context->UpdateSubresource1(
		m_constantBuffer.Get(),
		0,
		nullptr,
		&constants,
		0,
		0,
		0
	);

	UINT stride = sizeof(FrameVertex);
	UINT offset = 0;
	ID3D11Buffer* vertexBuffers[1] = { m_vertexBuffer.Get() };
	context->IASetVertexBuffers(0, 1, vertexBuffers, &stride, &offset);
	context->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	context->IASetInputLayout(m_inputLayout.Get());

	context->VSSetShader(m_vertexShader.Get(), nullptr, 0);
	context->VSSetConstantBuffers1(0, 1, m_constantBuffer.GetAddressOf(), nullptr, nullptr);
	context->PSSetShader(m_pixelShader.Get(), nullptr, 0);
	context->PSSetShaderResources(0, 1, m_frameTextureView.GetAddressOf());
	context->PSSetSamplers(0, 1, m_samplerState.GetAddressOf());
	context->Draw(4, 0);

	ID3D11ShaderResourceView* nullViews[1] = { nullptr };
	context->PSSetShaderResources(0, 1, nullViews);

	return true;
}
