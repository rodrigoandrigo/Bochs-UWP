#pragma once

#include "..\Common\DeviceResources.h"
#include "..\BochsUwpBridge.h"

namespace UWP_Port
{
	struct BochsFrameConstants
	{
		DirectX::XMFLOAT4 scaleOffset;
	};

	class BochsFrameRenderer
	{
	public:
		BochsFrameRenderer(const std::shared_ptr<DX::DeviceResources>& deviceResources);

		void CreateWindowSizeDependentResources();
		void CreateDeviceDependentResources();
		void ReleaseDeviceDependentResources();
		bool Render();

	private:
		void EnsureFrameTexture(const BochsFrameSnapshot& frame);
		void UploadFrameTexture(const BochsFrameSnapshot& frame);
		BochsFrameConstants BuildConstants(const BochsFrameSnapshot& frame) const;

		std::shared_ptr<DX::DeviceResources> m_deviceResources;
		Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vertexShader;
		Microsoft::WRL::ComPtr<ID3D11PixelShader> m_pixelShader;
		Microsoft::WRL::ComPtr<ID3D11InputLayout> m_inputLayout;
		Microsoft::WRL::ComPtr<ID3D11Buffer> m_vertexBuffer;
		Microsoft::WRL::ComPtr<ID3D11Texture2D> m_frameTexture;
		Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_frameTextureView;
		Microsoft::WRL::ComPtr<ID3D11SamplerState> m_samplerState;
		Microsoft::WRL::ComPtr<ID3D11Buffer> m_constantBuffer;
		unsigned m_textureWidth;
		unsigned m_textureHeight;
		bool m_loadingComplete;
	};
}
