/*
* Simd Library (http://ermig1979.github.io/Simd).
*
* Copyright (c) 2011-2021 Yermalayeu Ihar.
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
* SOFTWARE.
*/
#include "Simd/SimdMemory.h"
#include "Simd/SimdImageSave.h"
#include "Simd/SimdImageSaveJpeg.h"
#include "Simd/SimdLoad.h"
#include "Simd/SimdAvx2.h"

namespace Simd
{
#ifdef SIMD_AVX2_ENABLE    
    namespace Avx2
    {
        const uint32_t JpegZigZagTi32[64] = {
            0, 8, 1, 2, 9, 16, 24, 17,
            10, 3, 4, 11, 18, 25, 32, 40,
            33, 26, 19, 12, 5, 6, 13, 20,
            27, 34, 41, 48, 56, 49, 42, 35,
            28, 21, 14, 7, 15, 22, 29, 36,
            43, 50, 57, 58, 51, 44, 37, 30,
            23, 31, 38, 45, 52, 59, 60, 53,
            46, 39, 47, 54, 61, 62, 55, 63 };

        //---------------------------------------------------------------------

        static int JpegProcessDu(Base::BitBuf& bitBuf, float* CDU, int stride, const float* fdtbl, int DC, const uint16_t HTDC[256][2], const uint16_t HTAC[256][2])
        {
            SIMD_ALIGNED(32) int DUO[64], DU[64];
            JpegDct(CDU, stride, fdtbl, DUO);
            union
            {
                uint64_t u64[1];
                uint32_t u32[2];
                uint8_t u8[8];
            } dum;
            for (int i = 0, j = 0; i < 64; i += 8, j++)
            {
                __m256i du = _mm256_i32gather_epi32(DUO, _mm256_loadu_si256((__m256i*)(JpegZigZagTi32 + i)), 4);
                dum.u8[j] = ~_mm256_movemask_ps(_mm256_castsi256_ps(_mm256_cmpeq_epi32(du, Avx2::K_ZERO)));
                _mm256_storeu_si256((__m256i*)(DU + i), du);
            }
            int diff = DU[0] - DC;
            if (diff == 0)
                bitBuf.Push(HTDC[0]);
            else
            {
                uint16_t bits[2];
                Base::JpegCalcBits(diff, bits);
                bitBuf.Push(HTDC[bits[1]]);
                bitBuf.Push(bits);
            }
#if defined(SIMD_X64_ENABLE)
            if (dum.u64[0] == 0)
            {
                bitBuf.Push(HTAC[0x00]);
                return DU[0];
            }
            dum.u64[0] >>= 1;
            int i = 1;
            for (; dum.u64[0]; ++i, dum.u64[0] >>= 1)
            {
                int nrzeroes = (int)_tzcnt_u64(dum.u64[0]);
                i += nrzeroes;
                dum.u64[0] >>= nrzeroes;
                if (nrzeroes >= 16)
                {
                    for (int nrmarker = 16; nrmarker <= nrzeroes; nrmarker += 16)
                        bitBuf.Push(HTAC[0xF0]);
                    nrzeroes &= 15;
                }
                uint16_t bits[2];
                Base::JpegCalcBits(DU[i], bits);
                bitBuf.Push(HTAC[(nrzeroes << 4) + bits[1]]);
                bitBuf.Push(bits);
            }
            if (i < 64)
                bitBuf.Push(HTAC[0x00]);
#else
            int end0pos = 64;
            do
            {
                end0pos -= 8;
                int mask = ~_mm256_movemask_epi8(_mm256_cmpeq_epi32(_mm256_loadu_si256((__m256i*)(DU + end0pos)), Avx2::K_ZERO));
                if (mask)
                {
                    end0pos += 7 - _lzcnt_u32(mask) / 4;
                    break;
                }
            } 
            while (end0pos > 0);
            if (end0pos == 0)
            {
                bitBuf.Push(HTAC[0x00]);
                return DU[0];
            }
            for (int i = 1; i <= end0pos; ++i)
            {
                int startpos = i;
                for (; DU[i] == 0 && i <= end0pos; ++i);
                int nrzeroes = i - startpos;
                if (nrzeroes >= 16)
                {
                    int lng = nrzeroes >> 4;
                    int nrmarker;
                    for (nrmarker = 1; nrmarker <= lng; ++nrmarker)
                        bitBuf.Push(HTAC[0xF0]);
                    nrzeroes &= 15;
                }
                uint16_t bits[2];
                Base::JpegCalcBits(DU[i], bits);
                bitBuf.Push(HTAC[(nrzeroes << 4) + bits[1]]);
                bitBuf.Push(bits);
            }
            if (end0pos != 63)
                bitBuf.Push(HTAC[0x00]);
#endif
            return DU[0];
        }

        SIMD_INLINE void RgbToYuvInit(__m256 k[10])
        {
            k[0] = _mm256_set1_ps(+0.29900f);
            k[1] = _mm256_set1_ps(+0.58700f);
            k[2] = _mm256_set1_ps(+0.11400f);
            k[3] = _mm256_set1_ps(-128.000f);
            k[4] = _mm256_set1_ps(-0.16874f);
            k[5] = _mm256_set1_ps(-0.33126f);
            k[6] = _mm256_set1_ps(+0.50000f);
            k[7] = _mm256_set1_ps(+0.50000f);
            k[8] = _mm256_set1_ps(-0.41869f);
            k[9] = _mm256_set1_ps(-0.08131f);
        }

        SIMD_INLINE void RgbToYuv(const uint8_t* r, const uint8_t* g, const uint8_t* b, int stride, int height, 
            const __m256 k[10], float* y, float* u, float* v, int size)
        {
            for (int row = 0; row < size;)
            {
                for (int col = 0; col < size; col += 8)
                {
                    __m256 _r = _mm256_cvtepi32_ps(_mm256_cvtepu8_epi32(_mm_loadl_epi64((__m128i*)(r + col))));
                    __m256 _g = _mm256_cvtepi32_ps(_mm256_cvtepu8_epi32(_mm_loadl_epi64((__m128i*)(g + col))));
                    __m256 _b = _mm256_cvtepi32_ps(_mm256_cvtepu8_epi32(_mm_loadl_epi64((__m128i*)(b + col))));
                    _mm256_storeu_ps(y + col, _mm256_add_ps(_mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(_r, k[0]), _mm256_mul_ps(_g, k[1])), _mm256_mul_ps(_b, k[2])), k[3]));
                    //_mm256_storeu_ps(y + col, _mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(_r, _yr), _mm256_mul_ps(_g, _yg)), _mm256_add_ps(_mm256_mul_ps(_b, _yb), _yt)));
                    _mm256_storeu_ps(u + col, _mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(_r, k[4]), _mm256_mul_ps(_g, k[5])), _mm256_mul_ps(_b, k[6])));
                    _mm256_storeu_ps(v + col, _mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(_r, k[7]), _mm256_mul_ps(_g, k[8])), _mm256_mul_ps(_b, k[9])));
                }
                if(++row < height)
                    r += stride, g += stride, b += stride;
                y += size, u += size, v += size;
            }
        }

        SIMD_INLINE void GrayToY(const uint8_t* g, int stride, int height, const __m256 k[10], float* y, int size)
        {
            for (int row = 0; row < size;)
            {
                for (int col = 0; col < size; col += 8)
                {
                    __m256 _g = _mm256_cvtepi32_ps(_mm256_cvtepu8_epi32(_mm_loadl_epi64((__m128i*)(g + col))));
                    _mm256_storeu_ps(y + col, _mm256_add_ps(_g, k[3]));
                }
                if (++row < height)
                    g += stride;
                y += size;
            }
        }

        SIMD_INLINE void SubUv(const float * src, float * dst)
        {
            __m256 _0_25 = _mm256_set1_ps(0.25f), s0, s1;
            for (int yy = 0; yy < 8; yy += 1)
            {
                s0 = _mm256_add_ps(_mm256_loadu_ps(src + 0), _mm256_loadu_ps(src + 16));
                s1 = _mm256_add_ps(_mm256_loadu_ps(src + 8), _mm256_loadu_ps(src + 24));
                _mm256_storeu_ps(dst + 0, _mm256_mul_ps(PermutedHorizontalAdd(s0, s1), _0_25));
                src += 32;
                dst += 8;
            }
        }

        void JpegWriteBlockSubs(OutputMemoryStream& stream, int width, int height, const uint8_t* red,
            const uint8_t* green, const uint8_t* blue, int stride, const float* fY, const float* fUv, int dc[3])
        {
            __m256 k[10];
            RgbToYuvInit(k);
            int& DCY = dc[0], & DCU = dc[1], & DCV = dc[2];
            int width16 = width & (~15);
            bool gray = red == green && red == blue;
            Base::BitBuf bitBuf;
            for (int y = 0; y < height; y += 16)
            {
                int x = 0;
                SIMD_ALIGNED(16) float Y[256], U[256], V[256];
                SIMD_ALIGNED(16) float subU[64], subV[64];
                for (; x < width16; x += 16)
                {
                    if (gray)
                        GrayToY(red + x, stride, height - y, k, Y, 16);
                    else
                        RgbToYuv(red + x, green + x, blue + x, stride, height - y, k, Y, U, V, 16);
                    DCY = JpegProcessDu(bitBuf, Y + 0, 16, fY, DCY, Base::HuffmanYdc, Base::HuffmanYac);
                    DCY = JpegProcessDu(bitBuf, Y + 8, 16, fY, DCY, Base::HuffmanYdc, Base::HuffmanYac);
                    DCY = JpegProcessDu(bitBuf, Y + 128, 16, fY, DCY, Base::HuffmanYdc, Base::HuffmanYac);
                    DCY = JpegProcessDu(bitBuf, Y + 136, 16, fY, DCY, Base::HuffmanYdc, Base::HuffmanYac);
                    if (gray)
                        Base::JpegProcessDuGrayUv(bitBuf);
                    else
                    {
                        SubUv(U, subU);
                        SubUv(V, subV);
                        DCU = JpegProcessDu(bitBuf, subU, 8, fUv, DCU, Base::HuffmanUVdc, Base::HuffmanUVac);
                        DCV = JpegProcessDu(bitBuf, subV, 8, fUv, DCV, Base::HuffmanUVdc, Base::HuffmanUVac);
                    }
                    if (bitBuf.Full())
                    {
                        Avx2::WriteBits(stream, bitBuf.data, bitBuf.size);
                        bitBuf.Clear();
                    }
                }
                for (; x < width; x += 16)
                {
                    if (gray)
                        Base::GrayToY(red + x, stride, height - y, width - x, Y, 16);
                    else
                        Base::RgbToYuv(red + x, green + x, blue + x, stride, height - y, width - x, Y, U, V, 16);
                    DCY = JpegProcessDu(bitBuf, Y + 0, 16, fY, DCY, Base::HuffmanYdc, Base::HuffmanYac);
                    DCY = JpegProcessDu(bitBuf, Y + 8, 16, fY, DCY, Base::HuffmanYdc, Base::HuffmanYac);
                    DCY = JpegProcessDu(bitBuf, Y + 128, 16, fY, DCY, Base::HuffmanYdc, Base::HuffmanYac);
                    DCY = JpegProcessDu(bitBuf, Y + 136, 16, fY, DCY, Base::HuffmanYdc, Base::HuffmanYac);
                    if (gray)
                        Base::JpegProcessDuGrayUv(bitBuf);
                    else
                    {
                        SubUv(U, subU);
                        SubUv(V, subV);
                        DCU = JpegProcessDu(bitBuf, subU, 8, fUv, DCU, Base::HuffmanUVdc, Base::HuffmanUVac);
                        DCV = JpegProcessDu(bitBuf, subV, 8, fUv, DCV, Base::HuffmanUVdc, Base::HuffmanUVac);
                    }
                }
            }
            Avx2::WriteBits(stream, bitBuf.data, bitBuf.size);
            bitBuf.Clear();
        }

        void JpegWriteBlockFull(OutputMemoryStream& stream, int width, int height, const uint8_t* red,
            const uint8_t* green, const uint8_t* blue, int stride, const float* fY, const float* fUv, int dc[3])
        {
            __m256 k[10];
            RgbToYuvInit(k);
            int& DCY = dc[0], & DCU = dc[1], & DCV = dc[2];
            int width8 = width & (~7);
            bool gray = red == green && red == blue;
            Base::BitBuf bitBuf;
            for (int y = 0; y < height; y += 8)
            {
                int x = 0;
                SIMD_ALIGNED(16) float Y[64], U[64], V[64];
                for (; x < width8; x += 8)
                {
                    if (gray)
                        GrayToY(red + x, stride, height - y, k, Y, 8);
                    else
                        RgbToYuv(red + x, green + x, blue + x, stride, height - y, k, Y, U, V, 8);
                    DCY = JpegProcessDu(bitBuf, Y, 8, fY, DCY, Base::HuffmanYdc, Base::HuffmanYac);
                    if (gray)
                        Base::JpegProcessDuGrayUv(bitBuf);
                    else
                    {
                        DCU = JpegProcessDu(bitBuf, U, 8, fUv, DCU, Base::HuffmanUVdc, Base::HuffmanUVac);
                        DCV = JpegProcessDu(bitBuf, V, 8, fUv, DCV, Base::HuffmanUVdc, Base::HuffmanUVac);
                    }
                    if (bitBuf.Full())
                    {
                        Avx2::WriteBits(stream, bitBuf.data, bitBuf.size);
                        bitBuf.Clear();
                    }
                }
                for (; x < width; x += 8)
                {
                    if (gray)
                        Base::GrayToY(red + x, stride, height - y, width - x, Y, 8);
                    else
                        Base::RgbToYuv(red + x, green + x, blue + x, stride, height - y, width - x, Y, U, V, 8);
                    DCY = JpegProcessDu(bitBuf, Y, 8, fY, DCY, Base::HuffmanYdc, Base::HuffmanYac);
                    if (gray)
                        Base::JpegProcessDuGrayUv(bitBuf);
                    else
                    {
                        DCU = JpegProcessDu(bitBuf, U, 8, fUv, DCU, Base::HuffmanUVdc, Base::HuffmanUVac);
                        DCV = JpegProcessDu(bitBuf, V, 8, fUv, DCV, Base::HuffmanUVdc, Base::HuffmanUVac);
                    }
                }
                Avx2::WriteBits(stream, bitBuf.data, bitBuf.size);
                bitBuf.Clear();
            }
        }

        //---------------------------------------------------------------------

        ImageJpegSaver::ImageJpegSaver(const ImageSaverParam& param)
            : Sse41::ImageJpegSaver(param)
        {
        }

        void ImageJpegSaver::Init()
        {
            Sse41::ImageJpegSaver::Init();
            if (_param.width >= 32)
            {
                switch (_param.format)
                {
                case SimdPixelFormatBgr24:
                case SimdPixelFormatRgb24:
                    _deintBgr = Avx2::DeinterleaveBgr;
                    break;
                case SimdPixelFormatBgra32:
                case SimdPixelFormatRgba32:
                    _deintBgra = Avx2::DeinterleaveBgra;
                    break;
                }
            }
            _writeBlock = _subSample ? JpegWriteBlockSubs : JpegWriteBlockFull;
        }
    }
#endif// SIMD_AVX2_ENABLE
}