/**
 * Copyright (C) 2020 Samsung Electronics Co., Ltd. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *   http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 *
 * @file	fc_layer.cpp
 * @date	14 May 2020
 * @brief	This is Fully Connected Layer Class for Neural Network
 * @see		https://github.com/nnstreamer/nntrainer
 * @author	Jijoong Moon <jijoong.moon@samsung.com>
 * @bug		No known bugs except for NYI items
 *
 */

#include <common_properties.h>
#include <fc_layer.h>
#include <layer_context.h>
#include <lazy_tensor.h>
#include <nntrainer_error.h>
#include <nntrainer_log.h>
#include <node_exporter.h>
#include <util_func.h>

#include <iostream>

namespace nntrainer {

static constexpr size_t SINGLE_INOUT_IDX = 0;

enum FCParams { weight, bias };
enum LORAParams { loraA, loraB, loraW };

FullyConnectedLayer::FullyConnectedLayer() :
  LayerImpl(),
  fc_props(props::Unit(), props::LoraRank()){
  weight_idx.fill(std::numeric_limits<unsigned>::max());
}

void FullyConnectedLayer::finalize(InitLayerContext &context) {
  auto &weight_regularizer =
    std::get<props::WeightRegularizer>(*layer_impl_props);
  auto &weight_regularizer_constant =
    std::get<props::WeightRegularizerConstant>(*layer_impl_props);
  auto &weight_initializer =
    std::get<props::WeightInitializer>(*layer_impl_props);
  auto &weight_decay = std::get<props::WeightDecay>(*layer_impl_props);
  auto &bias_decay = std::get<props::BiasDecay>(*layer_impl_props);
  auto &bias_initializer = std::get<props::BiasInitializer>(*layer_impl_props);
  auto &disable_bias = std::get<props::DisableBias>(*layer_impl_props);

  auto unit = std::get<props::Unit>(fc_props).get();
  auto lora_rank = (std::get<props::LoraRank>(fc_props).empty())? 0 : std::get<props::LoraRank>(fc_props).get();

  NNTR_THROW_IF(context.getNumInputs() != 1, std::invalid_argument)
    << "Fully connected layer takes only one input";

  std::vector<TensorDim> output_dims(1);

  /// @todo fc actaully supports multidimensions. EffDimFlag shouldn't be fixed
  /// like this.
  context.setEffDimFlagInputDimension(0, 0b1001);
  context.setDynDimFlagInputDimension(0, 0b1000);

  bool is_nchw = (context.getFormat() == Tformat::NCHW);
  /** set output dimensions */
  auto const &in_dim = context.getInputDimensions()[0];
  output_dims[0] = in_dim;
  is_nchw ? output_dims[0].width(unit) : output_dims[0].channel(unit);

  output_dims[0].setTensorType(
    {context.getFormat(), context.getActivationDataType()});

  context.setOutputDimensions(output_dims);

  /** set weight specifications */
  // @todo : This NCHW format setting is just temporal, it needs to be set by
  // global configuration
  TensorDim bias_dim(
    1, is_nchw ? 1 : unit, 1, is_nchw ? unit : 1,
    TensorDim::TensorType(context.getFormat(), context.getWeightDataType()),
    is_nchw ? 0b0001 : 0b0100);

  TensorDim weight_dim(
    1, is_nchw ? 1 : unit, is_nchw ? in_dim.width() : 1,
    is_nchw ? unit : in_dim.channel(),
    TensorDim::TensorType(context.getFormat(), context.getWeightDataType()),
    is_nchw ? 0b0011 : 0b0101);

  weight_idx[FCParams::weight] = context.requestWeight(
    weight_dim, weight_initializer, weight_regularizer,
    weight_regularizer_constant, weight_decay, "weight", true);

  if (disable_bias.empty() || disable_bias.get() == false) {
    weight_idx[FCParams::bias] =
      context.requestWeight(bias_dim, bias_initializer, WeightRegularizer::NONE,
                            1.0f, bias_decay, "bias", true);
  }

  // create weights for LoRA
  if(lora_rank){

    /** set LoRA specifications */
    // loraA is a row vector (in_dim.width, lora_rank), loraB is a col vector of weight (lora_rank, out_dim),
    // where shape of (loraA @ loraB) = shape of (W)
    TensorDim loraA_dim(
      1, is_nchw ? 1 : lora_rank, is_nchw ? in_dim.width() : 1,
      is_nchw ? lora_rank : in_dim.channel(),
      TensorDim::TensorType(context.getFormat(), context.getWeightDataType()),
      is_nchw ? 0b0011 : 0b0101);

    TensorDim loraB_dim(
      1, is_nchw ? 1 : unit, is_nchw ? lora_rank : 1,
      is_nchw ? unit : in_dim.channel(),
      TensorDim::TensorType(context.getFormat(), context.getWeightDataType()),
      is_nchw ? 0b0011 : 0b0101);

    lora_idx[LORAParams::loraA] = context.requestWeight(
      loraA_dim, weight_initializer, weight_regularizer,
      weight_regularizer_constant, weight_decay, "loraA", true);

    lora_idx[LORAParams::loraB] = context.requestWeight(
      loraB_dim, weight_initializer, weight_regularizer,
      weight_regularizer_constant, weight_decay, "loraB", true);

    // save weight_lora as a tensor to be updated by (loraA @ loraB)
    lora_idx[LORAParams::loraW] = context.requestTensor(
      weight_dim, "weight_lora", Tensor::Initializer::NONE, true, 
      TensorLifespan::ITERATION_LIFESPAN);
  }

}

void FullyConnectedLayer::exportTo(
  Exporter &exporter, const ml::train::ExportMethods &method) const {
  LayerImpl::exportTo(exporter, method);
  exporter.saveResult(fc_props, method, this);
}

void FullyConnectedLayer::setProperty(const std::vector<std::string> &values) {
  auto remain_props = loadProperties(values, fc_props);
  LayerImpl::setProperty(remain_props);
}

/**
 * @brief forwarding function for lora. 
 * It would be called during `forwarding` of FC layer.
*/
void FullyConnectedLayer::forwarding_lora(RunLayerContext &context, Tensor &weight){
  Tensor &loraA = context.getWeight(lora_idx[LORAParams::loraA]);
  Tensor &loraB = context.getWeight(lora_idx[LORAParams::loraB]);
  Tensor &weight_lora = context.getTensor(lora_idx[LORAParams::loraW]);
  loraA.dot(loraB, weight_lora);   // weight_lora = loraA @ loraB
  weight.add(weight_lora, weight); // weight += weight_lora
}

void FullyConnectedLayer::forwarding(RunLayerContext &context, bool training) {
  Tensor &weight = context.getWeight(weight_idx[FCParams::weight]);
  Tensor &hidden_ = context.getOutput(SINGLE_INOUT_IDX);
  Tensor &input_ = context.getInput(SINGLE_INOUT_IDX);

  if (weight.getDataType() == nntrainer::Tdatatype::QINT4 ||
      weight.getDataType() == nntrainer::Tdatatype::QINT8) {
    Tdatatype dtype = input_.getDataType();

    Tensor weight_(
      {{weight.batch(), weight.channel(), weight.height(), weight.width()},
       {weight.getFormat(), dtype}},
      true);

    unsigned int axis =
      context.getWeightObject(weight_idx[FCParams::weight]).getOutputAxis();

    weight.dequantize(weight_, axis);
    if(!std::get<props::LoraRank>(fc_props).empty()) 
      forwarding_lora(context, weight_);
    input_.dot(weight_, hidden_, false, false);
  } else {
    if(!std::get<props::LoraRank>(fc_props).empty())
      forwarding_lora(context, weight);
    input_.dot(weight, hidden_, false, false);
  }

  if (auto &disable_bias = std::get<props::DisableBias>(*layer_impl_props);
      disable_bias.empty() || disable_bias.get() == false) {
    Tensor &bias = context.getWeight(weight_idx[FCParams::bias]);
    hidden_.add_i(bias);
  }
}

void FullyConnectedLayer::incremental_forwarding(RunLayerContext &context,
                                                 unsigned int from,
                                                 unsigned int to,
                                                 bool training) {
  Tensor w;
  Tensor &weight = w;
  context.getWeight(weight, weight_idx[FCParams::weight]);

  Tensor &input_ = context.getInput(SINGLE_INOUT_IDX);
  Tensor &hidden_ = context.getOutput(SINGLE_INOUT_IDX);

  TensorDim input_dim = input_.getDim();
  TensorDim hidden_dim = hidden_.getDim();

  TensorDim input_step_dim = input_dim;
  TensorDim hidden_step_dim = hidden_dim;

  if (from) {
    NNTR_THROW_IF(to - from != 1, std::invalid_argument)
      << "incremental step size is not 1";
    from = 0;
    to = 1;
  }

  input_step_dim.height(to - from);
  hidden_step_dim.height(to - from);

  // @todo: set reset stride as false. This implementation only works when batch
  // size is 1
  Tensor input_step = input_.getSharedDataTensor(input_step_dim, 0, true);
  Tensor hidden_step = hidden_.getSharedDataTensor(hidden_step_dim, 0, true);

  input_step.dot(weight, hidden_step, false, false);

  if (auto &disable_bias = std::get<props::DisableBias>(*layer_impl_props);
      disable_bias.empty() || disable_bias.get() == false) {
    Tensor &bias = context.getWeight(weight_idx[FCParams::bias]);
    hidden_step.add_i(bias);
  }
}

/**
 * @note 
 * [note for LoRA] implicit calcDerivative is implicitly applied. 
 * The weight is already updated with the LoRA's (W = W + W_lora)
*/
void FullyConnectedLayer::calcDerivative(RunLayerContext &context) {
  Tensor &weight = context.getWeight(weight_idx[FCParams::weight]);

  const Tensor &derivative_ = context.getIncomingDerivative(SINGLE_INOUT_IDX);
  Tensor &ret_ = context.getOutgoingDerivative(SINGLE_INOUT_IDX);

  ret_.dot_deriv_wrt_1(weight, derivative_, false, false);
}

void FullyConnectedLayer::calcGradient(RunLayerContext &context) {

  // (baseline) calcGradient
  if(std::get<props::LoraRank>(fc_props).empty()){
    Tensor &djdw = context.getWeightGrad(weight_idx[FCParams::weight]);

    const Tensor &derivative_ = context.getIncomingDerivative(SINGLE_INOUT_IDX);
    Tensor &input_ = context.getInput(SINGLE_INOUT_IDX);

    if (auto &disable_bias = std::get<props::DisableBias>(*layer_impl_props);
        disable_bias.empty() || disable_bias.get() == false) {
      Tensor &djdb = context.getWeightGrad(weight_idx[FCParams::bias]);

      if (context.isGradientFirstAccess(weight_idx[FCParams::bias])) {
        derivative_.sum({0, 1, 2}, djdb);
      } else {
        /// @todo optimize below by adding beta to Tensor::sum
        Tensor t = derivative_.sum({0, 1, 2});
        djdb.add_i(t);
      }
    }

    input_.dot_deriv_wrt_2(
      djdw, derivative_, false, false,
      !context.isGradientFirstAccess(weight_idx[FCParams::weight]));
  } else {
    // (LoRA) calcGradient
    Tensor &djdla = context.getWeightGrad(lora_idx[LORAParams::loraA]); 
    Tensor &djdlb = context.getWeightGrad(lora_idx[LORAParams::loraB]); 
    Tensor &djdlora_w = context.getTensorGrad(lora_idx[LORAParams::loraW]); 

    const Tensor &derivative_ = context.getIncomingDerivative(SINGLE_INOUT_IDX);
    Tensor &input_ = context.getInput(SINGLE_INOUT_IDX);
    Tensor &lora_A = context.getWeight(lora_idx[LORAParams::loraA]);
    Tensor &lora_B = context.getWeight(lora_idx[LORAParams::loraB]);

    // (cf) forward
      // input_.dot(lora_weight, hidden) : hidden = input @ lora_weight
      // lora_A.dot(lora_B, lora_weight) : lora_weight = lora_A @ lora_B
    input_.dot_deriv_wrt_2(
      djdlora_w, derivative_, false, false,
      !context.isGradientFirstAccess(lora_idx[LORAParams::loraW]));
    lora_A.dot_deriv_wrt_2(
      djdlb, djdlora_w, false, false,
      !context.isGradientFirstAccess(lora_idx[LORAParams::loraB]));
    djdla.dot_batched_deriv_wrt_1(
      lora_B, djdlora_w,false, false, 
      !context.isGradientFirstAccess(lora_idx[LORAParams::loraA]));
  }
}

} /* namespace nntrainer */
