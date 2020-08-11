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
#define RSBP_MSOP_ID (0xA050A55A0A05AA55)
#define RSBP_DIFOP_ID (0x555511115A00FFA5)
#define RSBP_BLOCK_ID (0xEEFF)
#define RSBP_BLOCKS_PER_PKT (12)
#define RSBP_CHANNELS_PER_BLOCK (32)
#define RSBP_CHANNEL_TOFFSET (3)
#define RSBP_FIRING_TDURATION (50)
const int RSBP_PKT_RATE = 1500;
const double RSBP_RX = 0.01473;
const double RSBP_RY = 0.0085;
const double RSBP_RZ = 0.09427;

#ifdef _MSC_VER
#pragma pack(push, 1)
#endif
typedef struct
{
  uint16_t id;
  uint16_t azimuth;
  RSChannel channels[RSBP_CHANNELS_PER_BLOCK];
}
#ifdef __GNUC__
__attribute__((packed))
#endif
RSBPMsopBlock;

typedef struct
{
  RSMsopHeader header;
  RSBPMsopBlock blocks[RSBP_BLOCKS_PER_PKT];
  unsigned int index;
  uint16_t tail;
}
#ifdef __GNUC__
__attribute__((packed))
#endif
RSBPMsopPkt;

typedef struct
{
  uint8_t reserved[240];
  uint8_t coef;
  uint8_t ver;
}
#ifdef __GNUC__
__attribute__((packed))
#endif
RSBPIntensity;

typedef struct
{
  uint64_t id;
  uint16_t rpm;
  RSEthNet eth;
  RSFOV fov;
  uint16_t reserved0;
  uint16_t phase_lock_angle;
  RSVersion version;
  RSBPIntensity intensity;
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
RSBPDifopPkt;

#ifdef _MSC_VER
#pragma pack(pop)
#endif

template <typename T_Point>
class DecoderRSBP : public DecoderBase<T_Point>
{
public:
  DecoderRSBP(const RSDecoderParam& param);
  RSDecoderResult decodeDifopPkt(const uint8_t* pkt);
  RSDecoderResult decodeMsopPkt(const uint8_t* pkt, std::vector<T_Point>& vec, int& height, int& azimuth);
  double getLidarTime(const uint8_t* pkt);

private:
  void initTable();

private:
  std::array<int, 32> beam_ring_table_;
};

template <typename T_Point>
DecoderRSBP<T_Point>::DecoderRSBP(const RSDecoderParam& param) : DecoderBase<T_Point>(param)
{
  this->angle_file_index_ = 32;
  if (this->param_.max_distance > 100.0f)
  {
    this->param_.max_distance = 100.0f;
  }
  if (this->param_.min_distance < 0.1f || this->param_.min_distance > this->param_.max_distance)
  {
    this->param_.min_distance = 0.1f;
  }
  initTable();
}

template <typename T_Point>
double DecoderRSBP<T_Point>::getLidarTime(const uint8_t* pkt)
{
  return this->template calculateTimeYMD<RSBPMsopPkt>(pkt);
}

template <typename T_Point>
RSDecoderResult DecoderRSBP<T_Point>::decodeMsopPkt(const uint8_t* pkt, std::vector<T_Point>& vec, int& height,
                                                    int& azimuth)
{
  height = RSBP_CHANNELS_PER_BLOCK;
  RSBPMsopPkt* mpkt_ptr = (RSBPMsopPkt*)pkt;
  if (mpkt_ptr->header.id != RSBP_MSOP_ID)
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
  for (size_t blk_idx = 0; blk_idx < RSBP_BLOCKS_PER_PKT; blk_idx++)
  {
    if (mpkt_ptr->blocks[blk_idx].id != RSBP_BLOCK_ID)
    {
      break;
    }
    int cur_azi = RS_SWAP_SHORT(mpkt_ptr->blocks[blk_idx].azimuth);
    float azi_diff = 0;
    if (this->echo_mode_ == ECHO_DUAL)
    {
      if (blk_idx < (RSBP_BLOCKS_PER_PKT - 2))  // 12
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
      if (blk_idx < (RSBP_BLOCKS_PER_PKT - 1))  // 12
      {
        azi_diff = (float)((36000 + RS_SWAP_SHORT(mpkt_ptr->blocks[blk_idx + 1].azimuth) - cur_azi) % 36000);
      }
      else
      {
        azi_diff = (float)((36000 + cur_azi - RS_SWAP_SHORT(mpkt_ptr->blocks[blk_idx - 1].azimuth)) % 36000);
      }
    }
    for (int channel_idx = 0; channel_idx < RSBP_CHANNELS_PER_BLOCK; channel_idx++)
    {
      float azi_channel_ori = cur_azi + (azi_diff * RSBP_CHANNEL_TOFFSET * (channel_idx % 16) / RSBP_FIRING_TDURATION);
      int azi_channel_final = this->azimuthCalibration(azi_channel_ori, channel_idx);
      float distance = RS_SWAP_SHORT(mpkt_ptr->blocks[blk_idx].channels[channel_idx].distance) * RS_RESOLUTION;
      int angle_horiz = (int)(azi_channel_ori + 36000) % 36000;
      int angle_vert = (((int)(this->vert_angle_list_[channel_idx]) % 36000) + 36000) % 36000;

      // store to point cloud buffer
      T_Point point;
      if ((distance <= this->param_.max_distance && distance >= this->param_.min_distance) &&
          ((this->angle_flag_ && azi_channel_final >= this->start_angle_ && azi_channel_final <= this->end_angle_) ||
           (!this->angle_flag_ &&
            ((azi_channel_final >= this->start_angle_) || (azi_channel_final <= this->end_angle_)))))
      {
        double x = distance * this->cos_lookup_table_[angle_vert] * this->cos_lookup_table_[azi_channel_final] +
                   RSBP_RX * this->cos_lookup_table_[angle_horiz];
        double y = -distance * this->cos_lookup_table_[angle_vert] * this->sin_lookup_table_[azi_channel_final] -
                   RSBP_RX * this->sin_lookup_table_[angle_horiz];
        double z = distance * this->sin_lookup_table_[angle_vert] + RSBP_RZ;
        double intensity = mpkt_ptr->blocks[blk_idx].channels[channel_idx].intensity;
        setX(point, x);
        setY(point, y);
        setZ(point, z);
        setIntensity(point, intensity);
        setRing(point, beam_ring_table_[channel_idx]);
      }
      else
      {
        setX(point, NAN);
        setY(point, NAN);
        setZ(point, NAN);
        setIntensity(point, 0);
        setRing(point, -1);
      }
      vec.emplace_back(std::move(point));
    }
  }
  return RSDecoderResult::DECODE_OK;
}

template <typename T_Point>
RSDecoderResult DecoderRSBP<T_Point>::decodeDifopPkt(const uint8_t* pkt)
{
  RSBPDifopPkt* dpkt_ptr = (RSBPDifopPkt*)pkt;
  if (dpkt_ptr->id != RSBP_DIFOP_ID)
  {
    return RSDecoderResult::WRONG_PKT_HEADER;
  }
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
    this->pkts_per_frame_ = ceil(2 * RSBP_PKT_RATE * 60 / RS_SWAP_SHORT(dpkt_ptr->rpm));
  }
  else
  {
    this->pkts_per_frame_ = ceil(RSBP_PKT_RATE * 60 / RS_SWAP_SHORT(dpkt_ptr->rpm));
  }

  if (!this->difop_flag_)
  {
    bool angle_flag = true;
    const uint8_t* p_ver_cali;
    p_ver_cali = ((RSBPDifopPkt*)pkt)->pitch_cali;
    if ((p_ver_cali[0] == 0x00 || p_ver_cali[0] == 0xFF) && (p_ver_cali[1] == 0x00 || p_ver_cali[1] == 0xFF) &&
        (p_ver_cali[2] == 0x00 || p_ver_cali[2] == 0xFF))
    {
      angle_flag = false;
    }
    if (angle_flag)
    {
      int lsb, mid, msb, neg = 1;
      const uint8_t* p_hori_cali = ((RSBPDifopPkt*)pkt)->yaw_cali;
      for (size_t i = 0; i < this->angle_file_index_; i++)
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
        this->vert_angle_list_[i] = (mid * 256 + msb) * neg;  // / 180 * M_PI;

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

        this->hori_angle_list_[i] = (mid * 256 + msb) * neg;
      }

      this->difop_flag_ = true;
    }
  }
  return RSDecoderResult::DECODE_OK;
}

template <typename T_Point>
void DecoderRSBP<T_Point>::initTable()
{
  beam_ring_table_[0] = 31;
  beam_ring_table_[1] = 28;
  beam_ring_table_[2] = 27;
  beam_ring_table_[3] = 25;
  beam_ring_table_[4] = 23;
  beam_ring_table_[5] = 21;
  beam_ring_table_[6] = 19;
  beam_ring_table_[7] = 17;
  beam_ring_table_[8] = 30;
  beam_ring_table_[9] = 29;
  beam_ring_table_[10] = 26;
  beam_ring_table_[11] = 24;
  beam_ring_table_[12] = 22;
  beam_ring_table_[13] = 20;
  beam_ring_table_[14] = 18;
  beam_ring_table_[15] = 16;
  beam_ring_table_[16] = 15;
  beam_ring_table_[17] = 13;
  beam_ring_table_[18] = 11;
  beam_ring_table_[19] = 9;
  beam_ring_table_[20] = 7;
  beam_ring_table_[21] = 5;
  beam_ring_table_[22] = 3;
  beam_ring_table_[23] = 1;
  beam_ring_table_[24] = 14;
  beam_ring_table_[25] = 12;
  beam_ring_table_[26] = 10;
  beam_ring_table_[27] = 8;
  beam_ring_table_[28] = 6;
  beam_ring_table_[29] = 4;
  beam_ring_table_[30] = 2;
  beam_ring_table_[31] = 0;
}

}  // namespace lidar
}  // namespace robosense
