/******************************************************************************
 * Copyright 2020 RoboSense All rights reserved.
 * Suteng Innovation Technology Co., Ltd. www.robosense.ai

 * This software is provided to you directly by RoboSense and might
 * only be used to access RoboSense LiDAR. Any compilation,
 * modification, exploration, reproduction and redistribution are
 * restricted without RoboSense's prior consent.

 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESSED OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL ROBOSENSE BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *****************************************************************************/
#include <rs_driver/driver/decoder/decoder_base.hpp>
namespace robosense
{
namespace lidar
{
#define RS32_CHANNELS_PER_BLOCK (32)
#define RS32_BLOCKS_PER_PKT (12)
#define RS32_MSOP_ID (0xA050A55A0A05AA55)
#define RS32_BLOCK_ID (0xEEFF)
#define RS32_DIFOP_ID (0x555511115A00FFA5)
#define RS32_CHANNEL_TOFFSET (3)
#define RS32_FIRING_TDURATION (50)
const int RS32_PKT_RATE = 1500;
const double RS32_RX = 0.03997;
const double RS32_RY = -0.01087;
const double RS32_RZ = 0;

#ifdef _MSC_VER
#pragma pack(push, 1)
#endif

typedef struct
{
  uint16_t id;
  uint16_t azimuth;
  RSChannel channels[RS32_CHANNELS_PER_BLOCK];
}
#ifdef __GNUC__
__attribute__((packed))
#endif
RS32MsopBlock;

typedef struct
{
  RSMsopHeader header;
  RS32MsopBlock blocks[RS32_BLOCKS_PER_PKT];
  uint32_t index;
  uint16_t tail;
}
#ifdef __GNUC__
__attribute__((packed))
#endif
RS32MsopPkt;

typedef struct
{
  uint8_t reserved[240];
  uint8_t coef;
  uint8_t ver;
}
#ifdef __GNUC__
__attribute__((packed))
#endif
RS32Intensity;

typedef struct
{
  uint64_t id;
  uint16_t rpm;
  RSEthNet eth;
  RSFOV fov;
  uint16_t reserved0;
  uint16_t phase_lock_angle;
  RSVersion version;
  RS32Intensity intensity;
  RSSn sn;
  uint16_t zero_cali;
  uint8_t return_mode;
  uint16_t sw_ver;
  RSTimestamp timestamp;
  RSStatus status;
  uint8_t reserved1[5];
  RSDiagno diagno;
  uint8_t gprmc[86];
  uint8_t pitch_cali[96];
  uint8_t yaw_cali[96];
  uint8_t reserved2[586];
  uint16_t tail;
}
#ifdef __GNUC__
__attribute__((packed))
#endif
RS32DifopPkt;

#ifdef _MSC_VER
#pragma pack(pop)
#endif

template <typename T_Point>
class DecoderRS32 : public DecoderBase<T_Point>
{
public:
  DecoderRS32(const RSDecoderParam& param);
  RSDecoderResult decodeDifopPkt(const uint8_t* pkt);
  RSDecoderResult decodeMsopPkt(const uint8_t* pkt, std::vector<T_Point>& vec, int& height, int& azimuth);
  double getLidarTime(const uint8_t* pkt);
};

template <typename T_Point>
DecoderRS32<T_Point>::DecoderRS32(const RSDecoderParam& param) : DecoderBase<T_Point>(param)
{
  this->angle_file_index_ = 32;
  if (this->param_.max_distance > 200.0f)
  {
    this->param_.max_distance = 200.0f;
  }
  if (this->param_.min_distance < 0.4f || this->param_.min_distance > this->param_.max_distance)
  {
    this->param_.min_distance = 0.4f;
  }
}

template <typename T_Point>
double DecoderRS32<T_Point>::getLidarTime(const uint8_t* pkt)
{
  RS32MsopPkt* mpkt_ptr = (RS32MsopPkt*)pkt;
  std::tm stm;
  memset(&stm, 0, sizeof(stm));
  stm.tm_year = mpkt_ptr->header.timestamp.year + 100;
  stm.tm_mon = mpkt_ptr->header.timestamp.month - 1;
  stm.tm_mday = mpkt_ptr->header.timestamp.day;
  stm.tm_hour = mpkt_ptr->header.timestamp.hour;
  stm.tm_min = mpkt_ptr->header.timestamp.minute;
  stm.tm_sec = mpkt_ptr->header.timestamp.second;
  return std::mktime(&stm) + (double)RS_SWAP_SHORT(mpkt_ptr->header.timestamp.ms) / 1000.0 +
         (double)RS_SWAP_SHORT(mpkt_ptr->header.timestamp.us) / 1000000.0;
}

template <typename T_Point>
RSDecoderResult DecoderRS32<T_Point>::decodeMsopPkt(const uint8_t* pkt, std::vector<T_Point>& vec, int& height,
                                                    int& azimuth)
{
  height = 32;
  RS32MsopPkt* mpkt_ptr = (RS32MsopPkt*)pkt;
  if (mpkt_ptr->header.id != RS32_MSOP_ID)
  {
    return RSDecoderResult::WRONG_PKT_HEADER;
  }
  this->current_temperature_ = this->computeTemperature(mpkt_ptr->header.temp_raw);
  int first_azimuth = RS_SWAP_SHORT(mpkt_ptr->blocks[0].azimuth);
  azimuth = first_azimuth;
  if (this->trigger_flag_)
  {
    if (this->param_.use_lidar_clock)
    {
      this->checkTriggerAngle(first_azimuth, getLidarTime(pkt));
    }
    else
    {
      this->checkTriggerAngle(first_azimuth, getTime());
    }
  }

  for (int blk_idx = 0; blk_idx < RS32_BLOCKS_PER_PKT; blk_idx++)
  {
    if (mpkt_ptr->blocks[blk_idx].id != RS32_BLOCK_ID)
    {
      break;
    }
    int cur_azi = RS_SWAP_SHORT(mpkt_ptr->blocks[blk_idx].azimuth);
    float azi_diff = 0;
    if (this->echo_mode_ == ECHO_DUAL)
    {
      if (blk_idx < (RS32_BLOCKS_PER_PKT - 2))  // 12
      {
        azi_diff = (float)((36000 + RS_SWAP_SHORT(mpkt_ptr->blocks[blk_idx + 2].azimuth) - cur_azi) % 36000);
      }
      else
      {
        azi_diff = (float)((36000 + cur_azi - RS_SWAP_SHORT(mpkt_ptr->blocks[blk_idx - 2].azimuth)) % 36000);
      }
    }
    else
    {
      if (blk_idx < (RS32_BLOCKS_PER_PKT - 1))  // 12
      {
        azi_diff = (float)((36000 + RS_SWAP_SHORT(mpkt_ptr->blocks[blk_idx + 1].azimuth) - cur_azi) % 36000);
      }
      else
      {
        azi_diff = (float)((36000 + cur_azi - RS_SWAP_SHORT(mpkt_ptr->blocks[blk_idx - 1].azimuth)) % 36000);
      }
    }
    float azimuth_channel;
    for (int channel_idx = 0; channel_idx < RS32_CHANNELS_PER_BLOCK; channel_idx++)
    {
      azimuth_channel = cur_azi + (azi_diff * RS32_CHANNEL_TOFFSET * (channel_idx % 16) / RS32_FIRING_TDURATION);
      int azimuth_final = this->azimuthCalibration(azimuth_channel, channel_idx);

      int distance = RS_SWAP_SHORT(mpkt_ptr->blocks[blk_idx].channels[channel_idx].distance);
      float distance_cali = distance * RS_RESOLUTION;

      int angle_horiz_ori = (int)(azimuth_channel + 36000) % 36000;
      int angle_vert = (((int)(this->vert_angle_list_[channel_idx]) % 36000) + 36000) % 36000;

      // store to point cloud buffer
      T_Point point;
      if ((distance_cali <= this->param_.max_distance && distance_cali >= this->param_.min_distance) &&
          ((this->angle_flag_ && azimuth_final >= this->start_angle_ && azimuth_final <= this->end_angle_) ||
           (!this->angle_flag_ && ((azimuth_final >= this->start_angle_) || (azimuth_final <= this->end_angle_)))))
      {
        point.x = distance_cali * this->cos_lookup_table_[angle_vert] * this->cos_lookup_table_[azimuth_final] +
                  RS32_RX * this->cos_lookup_table_[angle_horiz_ori];
        point.y = -distance_cali * this->cos_lookup_table_[angle_vert] * this->sin_lookup_table_[azimuth_final] -
                  RS32_RX * this->sin_lookup_table_[angle_horiz_ori];
        point.z = distance_cali * this->sin_lookup_table_[angle_vert] + RS32_RZ;
        point.intensity = mpkt_ptr->blocks[blk_idx].channels[channel_idx].intensity;
        if (std::isnan(point.intensity))
        {
          point.intensity = 0;
        }
      }
      else
      {
        point.x = NAN;
        point.y = NAN;
        point.z = NAN;
        point.intensity = NAN;
      }
      vec.emplace_back(std::move(point));
    }
  }
  return RSDecoderResult::DECODE_OK;
}

template <typename T_Point>
RSDecoderResult DecoderRS32<T_Point>::decodeDifopPkt(const uint8_t* pkt)
{
  RS32DifopPkt* dpkt_ptr = (RS32DifopPkt*)pkt;
  if (dpkt_ptr->id != RS32_DIFOP_ID)
  {
    return RSDecoderResult::WRONG_PKT_HEADER;
  }
  this->rpm_ = RS_SWAP_SHORT(dpkt_ptr->rpm);

  switch (dpkt_ptr->return_mode)
  {
    case 0x00:
      this->echo_mode_ = RSEchoMode::ECHO_DUAL;
      break;
    case 0x01:
      this->echo_mode_ = RSEchoMode::ECHO_STRONGEST;
      break;
    case 0x02:
      this->echo_mode_ = RSEchoMode::ECHO_LAST;
      break;
    default:
      break;
  }
  if (this->echo_mode_ == ECHO_DUAL)
  {
    this->pkts_per_frame_ = ceil(2 * RS32_PKT_RATE * 60 / this->rpm_);
  }
  else
  {
    this->pkts_per_frame_ = ceil(RS32_PKT_RATE * 60 / this->rpm_);
  }

  if (!this->difop_flag_)
  {
    bool angle_flag = true;
    const uint8_t* p_ver_cali;
    p_ver_cali = ((RS32DifopPkt*)pkt)->pitch_cali;
    if ((p_ver_cali[0] == 0x00 || p_ver_cali[0] == 0xFF) && (p_ver_cali[1] == 0x00 || p_ver_cali[1] == 0xFF) &&
        (p_ver_cali[2] == 0x00 || p_ver_cali[2] == 0xFF))
    {
      angle_flag = false;
    }
    if (angle_flag)
    {
      int lsb, mid, msb, neg = 1;
      const uint8_t* p_hori_cali = ((RS32DifopPkt*)pkt)->yaw_cali;
      for (int i = 0; i < 32; i++)
      {
        /* vert angle calibration data */
        lsb = p_ver_cali[i * 3];
        mid = p_ver_cali[i * 3 + 1];
        msb = p_ver_cali[i * 3 + 2];
        if (lsb == 0)
        {
          neg = 1;
        }
        else if (lsb == 1)
        {
          neg = -1;
        }
        this->vert_angle_list_[i] = (mid * 256 + msb) * neg * 0.1f;  // / 180 * M_PI;

        /* horizon angle calibration data */
        lsb = p_hori_cali[i * 3];
        mid = p_hori_cali[i * 3 + 1];
        msb = p_hori_cali[i * 3 + 2];
        if (lsb == 0)
        {
          neg = 1;
        }
        else if (lsb == 1)
        {
          neg = -1;
        }

        this->hori_angle_list_[i] = (mid * 256 + msb) * neg * 0.1f;
      }
      this->difop_flag_ = true;
    }
  }
  return RSDecoderResult::DECODE_OK;
}

}  // namespace lidar
}  // namespace robosense