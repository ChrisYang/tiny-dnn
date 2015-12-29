/*
    Copyright (c) 2013, Taiga Nomi
    All rights reserved.
    
    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.
    * Neither the name of the <organization> nor the
    names of its contributors may be used to endorse or promote products
    derived from this software without specific prior written permission.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY 
    EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED 
    WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE 
    DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY 
    DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES 
    (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; 
    LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND 
    ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT 
    (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS 
    SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/
#pragma once
#include <algorithm>
#include <memory>
#include <unordered_map>
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>
#include <google/protobuf/text_format.h>
#include <io.h>
#include <fcntl.h>
#include <limits>
#include "caffe.pb.h"
#include "tiny_cnn/tiny_cnn.h"

typedef tiny_cnn::layer_shape_t shape_t;

#ifdef _WIN32
#define _NOMINMAX
#include <io.h>
#ifndef O_RDONLY
#define _O_RDONLY O_RDONLY
#endif
#endif

namespace tiny_cnn {
namespace detail {

inline std::shared_ptr<tiny_cnn::weight_init::function> create_filler(const std::string& filler) {
    if (filler == "xavier") {
        return std::make_shared<tiny_cnn::weight_init::xavier>();
    }
    else if (filler == "constant") {
        return std::make_shared<tiny_cnn::weight_init::constant>();
    }
    else if (filler == "gaussian") {
        return std::make_shared<tiny_cnn::weight_init::gaussian>();
    }
    else {
        throw std::runtime_error("unsupported filler type");
    }
}

template <typename param>
inline bool get_kernel_size_2d(const param& p, tiny_cnn::layer_size_t *kernel) {
    if (p.has_kernel_w() && p.has_kernel_w()) {
        if (p.kernel_w() != p.kernel_h())
            throw std::runtime_error("unsupported kernel shape");
        *kernel = p.kernel_w();
        return true;
    }
    return false;
}

inline std::shared_ptr<tiny_cnn::layer_base> create_max_pool(int pool_size, int stride, const shape_t& bottom_shape, shape_t *top_shape)
{
    using max_pool = tiny_cnn::max_pooling_layer<tiny_cnn::activation::identity>;
    auto mp = std::make_shared<max_pool>(bottom_shape.width_, bottom_shape.height_, bottom_shape.depth_, pool_size, stride);
    *top_shape = mp->out_shape();
    return mp;
}

inline std::shared_ptr<tiny_cnn::layer_base> create_ave_pool(int pool_size, int stride, const shape_t& bottom_shape, shape_t *top_shape)
{
    using ave_pool = tiny_cnn::average_pooling_layer<tiny_cnn::activation::identity>;
    auto ap = std::make_shared<ave_pool>(bottom_shape.width_, bottom_shape.height_, bottom_shape.depth_, pool_size, stride);

    // tiny-cnn has trainable parameter in average-pooling layer
    tiny_cnn::float_t weight = 1.0 / tiny_cnn::sqr(pool_size);
    std::fill(ap->weight().begin(), ap->weight().end(), weight);
    std::fill(ap->bias().begin(), ap->bias().end(), (tiny_cnn::float_t)0.0);
    *top_shape = ap->out_shape();
    return ap;
}

inline std::shared_ptr<tiny_cnn::layer_base> create_softmax(const caffe::LayerParameter& layer, const shape_t& bottom_shape, shape_t *) {
    auto sm = std::make_shared<tiny_cnn::linear_layer<tiny_cnn::activation::softmax>>(bottom_shape.size());
    return sm;
}

inline std::shared_ptr<tiny_cnn::layer_base> create_sigmoid(const caffe::LayerParameter& layer, const shape_t& bottom_shape, shape_t *) {
    auto ce = std::make_shared<tiny_cnn::linear_layer<tiny_cnn::activation::sigmoid>>(bottom_shape.size());
    return ce;
}

inline std::shared_ptr<tiny_cnn::layer_base> create_tanh(const caffe::LayerParameter& layer, const shape_t& bottom_shape, shape_t *) {
    auto tanh = std::make_shared<tiny_cnn::linear_layer<tiny_cnn::activation::tan_h>>(bottom_shape.size());
    return tanh;
}

inline std::shared_ptr<tiny_cnn::layer_base> create_pooling(const caffe::LayerParameter& layer, const shape_t& bottom_shape, shape_t *top_shape) {
    using max_pool = tiny_cnn::max_pooling_layer<tiny_cnn::activation::identity>;
    using ave_pool = tiny_cnn::average_pooling_layer<tiny_cnn::activation::identity>;

    if (!layer.has_pooling_param())
        throw std::runtime_error("pool param missing");

    auto pool_param = layer.pooling_param();

    tiny_cnn::layer_size_t pool_size = 0;
    tiny_cnn::layer_size_t h_stride, w_stride;

    if (!get_kernel_size_2d(pool_param, &pool_size))
        pool_size = pool_param.kernel_size();

    if (pool_param.has_stride() || pool_param.has_stride_h())
        h_stride = pool_param.has_stride() ? pool_param.stride() : pool_param.stride_h();

    if (pool_param.has_stride() || pool_param.has_stride_w())
        w_stride = pool_param.has_stride() ? pool_param.stride() : pool_param.stride_w();

    if (h_stride != w_stride)// || h_stride != pool_size)
        throw std::runtime_error("unsupported pool shape");

    if (pool_param.has_pool()) {
        auto type = pool_param.pool();

        switch (type) {
        case caffe::PoolingParameter_PoolMethod_MAX: return create_max_pool(pool_size, h_stride, bottom_shape, top_shape);
        case caffe::PoolingParameter_PoolMethod_AVE: return create_ave_pool(pool_size, h_stride, bottom_shape, top_shape);
        default: throw std::runtime_error("unsupported layer type");
        }
    }
    // default:max-pool
    return create_max_pool(pool_size, h_stride, bottom_shape, top_shape);
}

inline std::shared_ptr<tiny_cnn::layer_base> create_relu(const caffe::LayerParameter& layer, const shape_t& bottom_shape, shape_t *) {
    auto relu = std::make_shared<tiny_cnn::linear_layer<tiny_cnn::activation::relu>>(bottom_shape.size());
    return relu;
}


inline void load_weights_fullyconnected(const caffe::LayerParameter& src, tiny_cnn::layer_base *dst)
{
    auto weights = src.blobs(0);
    int curr = 0;

    if (dst->out_size() * dst->in_size() != weights.data_size())
        throw std::runtime_error(std::string("layer size mismatch!") +
            "caffe(" + src.name() + "):" + std::to_string(weights.data_size()) + "\n" +
            "tiny-cnn(" + dst->layer_type() + "):" + std::to_string(dst->weight().size()));

    for (size_t o = 0; o < dst->out_size(); o++)
        for (size_t i = 0; i < dst->in_size(); i++)
            dst->weight()[i * dst->out_size() + o] = weights.data(curr++); // transpose

                                                                        // fill bias
    if (src.inner_product_param().bias_term()) {
        auto biases = src.blobs(1);
        for (size_t o = 0; o < dst->out_size(); o++)
            dst->bias()[o] = biases.data(o);
    }
}

inline std::shared_ptr<tiny_cnn::layer_base> create_fullyconnected(const caffe::LayerParameter& layer, const shape_t& bottom_shape, shape_t *top_shape) {
    using fc_layer = tiny_cnn::fully_connected_layer<tiny_cnn::activation::identity>;

    if (!layer.has_inner_product_param())
        throw std::runtime_error("inner-product param missing");

    tiny_cnn::layer_size_t dim_input = 0, dim_output = 0;
    bool has_bias = true;

    auto ip_param = layer.inner_product_param();
    has_bias = ip_param.bias_term();

    dim_output = ip_param.num_output();
    dim_input = bottom_shape.size();

    auto ip = std::make_shared<fc_layer>(dim_input, dim_output, has_bias);

    // filler
    if (ip_param.has_weight_filler())
        ip->weight_init(create_filler(ip_param.weight_filler().type()));

    if (ip_param.has_bias_filler())
        ip->bias_init(create_filler(ip_param.bias_filler().type()));

    // weight
    if (layer.blobs_size() > 0) {
        load_weights_fullyconnected(layer, ip.get());
    }
    *top_shape = ip->out_shape();
    return ip;
}

inline void load_weights_conv(const caffe::LayerParameter& src, tiny_cnn::layer_base *dst)
{
    // fill weight
    auto weights = src.blobs(0);

    /*for (size_t o = 0; o < out_channels; o++)
        for (size_t i = 0; i < in_channels; i++)
            for (size_t y = 0; y < window_size; y++)
                for (size_t x = 0; x < window_size; x++)
                    conv->weight_at(i, o, x, y) = weights.data(curr++);
                    */

    for (int o = 0; o < weights.data_size(); o++) {
        dst->weight()[o] = weights.data(o);
    }

    // fill bias
    if (src.convolution_param().bias_term()) {
        auto biases = src.blobs(1);
        for (int o = 0; o < biases.data_size(); o++)
            dst->bias()[o] = biases.data(o);
    }
}

inline void load_weights_pool(const caffe::LayerParameter& src, tiny_cnn::layer_base *dst)
{
    auto pool_param = src.pooling_param();

    if (dst->weight().size()) {
        tiny_cnn::layer_size_t pool_size = 0;

        if (!get_kernel_size_2d(pool_param, &pool_size))
            pool_size = pool_param.kernel_size();

        // tiny-cnn has trainable parameter in average-pooling layer
        tiny_cnn::float_t weight = 1.0 / tiny_cnn::sqr(pool_size);
        if (!dst->weight().empty()) std::fill(dst->weight().begin(), dst->weight().end(), weight);
        if (!dst->bias().empty()) std::fill(dst->bias().begin(), dst->bias().end(), (tiny_cnn::float_t)0.0);
    }
}

inline std::shared_ptr<tiny_cnn::layer_base> create_lrn(const caffe::LayerParameter& layer, const shape_t& bottom_shape, shape_t *top_shape) {
    using lrn_layer = tiny_cnn::lrn_layer<tiny_cnn::activation::identity>;

    if (!layer.has_lrn_param())
        throw std::runtime_error("lrn param missing");

    auto lrn_param = layer.lrn_param();
    layer_size_t local_size = 5;
    float_t alpha = 1;
    float_t beta = 5;
    norm_region region = norm_region::across_channels;

    if (lrn_param.has_local_size()) local_size = lrn_param.local_size();
    if (lrn_param.has_alpha()) alpha = lrn_param.alpha();
    if (lrn_param.has_beta()) beta = lrn_param.beta();
    if (lrn_param.has_norm_region()) {
        if (lrn_param.norm_region() == caffe::LRNParameter_NormRegion_WITHIN_CHANNEL)
            region = norm_region::within_channels;
    }

    auto lrn = std::make_shared<lrn_layer>(bottom_shape.width_, bottom_shape.height_, local_size, bottom_shape.depth_, alpha, beta, region);

    return lrn;
}

inline std::shared_ptr<tiny_cnn::layer_base> create_dropout(const caffe::LayerParameter& layer, const shape_t& bottom_shape, shape_t *top_shape) {
    if (!layer.has_dropout_param())
        throw std::runtime_error("dropout param missing");

    float_t dropout_rate = 0.5;

    if (layer.dropout_param().has_dropout_ratio())
        dropout_rate = layer.dropout_param().dropout_ratio();

    auto dropout = std::make_shared<dropout_layer>(bottom_shape.size(), dropout_rate, net_phase::test);

    return dropout;

}

inline std::shared_ptr<tiny_cnn::layer_base> create_convlayer(const caffe::LayerParameter& layer, const shape_t& bottom_shape, shape_t *top_shape) {
    using conv_layer = tiny_cnn::convolutional_layer<tiny_cnn::activation::identity>;

    if (!layer.has_convolution_param())
        throw std::runtime_error("convolution param missing");

    // layer parameters
    tiny_cnn::layer_size_t in_width = 0, in_height = 0, window_size = 0, in_channels = 0, out_channels = 0;
    tiny_cnn::layer_size_t w_stride = 1, h_stride = 1;
    bool has_bias = true;
    tiny_cnn::padding pad_type = tiny_cnn::padding::valid;
    tiny_cnn::connection_table table;

    auto conv_param = layer.convolution_param();

    // shape  
    out_channels = conv_param.num_output();
    in_channels = bottom_shape.depth_;
    in_width = bottom_shape.width_;
    in_height = bottom_shape.height_;
    has_bias = conv_param.bias_term();
    window_size = 0;

    if (!get_kernel_size_2d(conv_param, &window_size)) {
        if (conv_param.kernel_size_size() > 1)
            throw std::runtime_error("unsupported kernel shape");
        window_size = conv_param.kernel_size(0);
    }

    // padding
    if (conv_param.pad_size() == 1 || (conv_param.has_pad_w() && conv_param.has_pad_h())) {
        uint32_t pad_w = conv_param.pad_size() == 1 ? conv_param.pad(0) : conv_param.pad_w();
        uint32_t pad_h = conv_param.pad_size() == 1 ? conv_param.pad(0) : conv_param.pad_h();

        if (pad_w != pad_h)
            throw std::runtime_error("conv:not supported padding size");

        // 0 ... valid, (window_size-1)/2 ... same
        if (pad_w == (window_size - 1) / 2) {
            pad_type = tiny_cnn::padding::same;
        }
        else if (pad_w == 0) {
            pad_type = tiny_cnn::padding::valid;
        }
        else {
            throw std::runtime_error("conv:not supported padding size");
        }
    }

    // stride
    if (conv_param.stride_size() == 1 || conv_param.has_stride_h())
        h_stride = conv_param.stride_size() == 1 ? conv_param.stride(0) : conv_param.stride_h();

    if (conv_param.stride_size() == 1 || conv_param.has_stride_w())
        w_stride = conv_param.stride_size() == 1 ? conv_param.stride(0) : conv_param.stride_w();

    // group
    if (conv_param.has_group())
        table = tiny_cnn::connection_table(conv_param.group(), in_channels, out_channels);

    auto conv = std::make_shared<conv_layer>(in_width, in_height, window_size, in_channels, out_channels, table, pad_type, has_bias, w_stride, h_stride);

    // filler
    if (conv_param.has_weight_filler())
        conv->weight_init(create_filler(conv_param.weight_filler().type()));

    if (conv_param.has_bias_filler())
        conv->bias_init(create_filler(conv_param.bias_filler().type()));

    // set weight (optional)
    if (layer.blobs_size() > 0) { // blobs(0)...weight, blobs(1)...bias

        // fill weight
        auto weights = layer.blobs(0);

        int dim = weights.data_size();
        int curr = 0;

        for (size_t o = 0; o < out_channels; o++) {
            for (size_t i = 0; i < in_channels; i++) {
                if (!table.is_connected(o, i))
                    continue;
                for (size_t y = 0; y < window_size; y++) {
                    for (size_t x = 0; x < window_size; x++) {
                        conv->weight_at(i, o, x, y) = weights.data(curr++);
                    }
                }
            }
        }

        if (curr != dim)
            throw std::runtime_error("weight dimension mismatch");
        //// fill bias
        if (has_bias) {
            auto biases = layer.blobs(1);
            for (size_t o = 0; o < out_channels; o++)
                conv->bias()[o] = biases.data(o);
        }
    }
    *top_shape = conv->out_shape();
    return conv;
}

inline bool layer_skipped(const std::string& type) {
    if (type == "Data" || type == "EuclideanLoss") return true;
    return false;
}

inline bool layer_has_weights(const std::string& type) {
    static const char* activations[] =
    {
        "SoftmaxWithLoss", "SigmoidCrossEntropyLoss", "LRN", "Dropout",
        "ReLU", "Sigmoid", "TanH", "Softmax"
    };
    for (int i = 0; i < sizeof(activations) / sizeof(activations[0]); i++) {
        if (activations[i] == type) return false;
    }
    return true;
}

inline bool layer_supported(const std::string& type) {
    static const char* supported[] =
    {
        "InnerProduct", "Convolution", "Pooling", "LRN", "Dropout",
        "SoftmaxWithLoss", "SigmoidCrossEntropyLoss",
        "ReLU", "Sigmoid", "TanH", "Softmax"
    };

    for (size_t i = 0; i < sizeof(supported) / sizeof(supported[0]); i++) {
        if (supported[i] == type) return true;
    }
    return false;
}

inline bool layer_match(const std::string& caffetype, const std::string& tiny_cnn_type) {
    const char* conversions[][2] =
    {
        { "InnerProduct", "fully-connected" },
        { "Convolution", "conv" },
        { "Pooling", "ave-pool" },
        { "Pooling", "max-pool" }
    };

    for (size_t i = 0; i < sizeof(conversions) / sizeof(conversions[0]); i++) {
        if (conversions[i][0] == caffetype && conversions[i][1] == tiny_cnn_type) return true;
    }
    return false;
}

inline std::shared_ptr<tiny_cnn::layer_base> create(const caffe::LayerParameter& layer, const shape_t &in_shape, shape_t *out_shape) {
    typedef std::function<std::shared_ptr<tiny_cnn::layer_base>(const caffe::LayerParameter&, const shape_t&, shape_t*)> factoryimpl;

    std::unordered_map<std::string, factoryimpl> factory_registry;

    factory_registry["Convolution"] = tiny_cnn::detail::create_convlayer;
    factory_registry["InnerProduct"] = tiny_cnn::detail::create_fullyconnected;
    factory_registry["Pooling"] = tiny_cnn::detail::create_pooling;
    factory_registry["LRN"] = tiny_cnn::detail::create_lrn;
    factory_registry["Dropout"] = tiny_cnn::detail::create_dropout;
    factory_registry["SoftmaxWithLoss"] = tiny_cnn::detail::create_softmax;
    factory_registry["SigmoidCrossEntropyLoss"] = tiny_cnn::detail::create_sigmoid;
    factory_registry["ReLU"] = tiny_cnn::detail::create_relu;
    factory_registry["Sigmoid"] = tiny_cnn::detail::create_sigmoid;
    factory_registry["TanH"] = tiny_cnn::detail::create_tanh;
    factory_registry["Softmax"] = tiny_cnn::detail::create_tanh;

    if (factory_registry.find(layer.type()) == factory_registry.end())
        throw std::runtime_error("layer parser not found");

    return factory_registry[layer.type()](layer, in_shape, out_shape);
}

inline void load(const caffe::LayerParameter& src, tiny_cnn::layer_base *dst) {
    typedef std::function<void(const caffe::LayerParameter&, tiny_cnn::layer_base*)> factoryimpl;
    std::unordered_map<std::string, factoryimpl> factory_registry;

    factory_registry["Convolution"] = tiny_cnn::detail::load_weights_conv;
    factory_registry["InnerProduct"] = tiny_cnn::detail::load_weights_fullyconnected;
    factory_registry["Pooling"] = tiny_cnn::detail::load_weights_pool;

    if (factory_registry.find(src.type()) == factory_registry.end())
        throw std::runtime_error("layer parser not found");

    return factory_registry[src.type()](src, dst);
}


struct layer_node {
    const caffe::LayerParameter *layer;
    const layer_node *next; // top-side
    const layer_node *prev; // bottom-side

    layer_node() : layer(0), next(0), prev(0) {}
    layer_node(const caffe::LayerParameter *l) : layer(l), next(0), prev(0) {}
};

// parse caffe net and interpret as single layer vector
class caffe_layer_vector {
public:
    caffe_layer_vector(const caffe::NetParameter& net_orig) : net(net_orig) {
        if (net.layers_size() > 0)
            upgradev1net(net_orig, &net);

        nodes.reserve(net.layer_size());

        for (int i = 0; i < net.layer_size(); i++) {
            auto& l = net.layer(i);
            nodes.emplace_back(&l);
            layer_table[l.name()] = &nodes.back();
        }

        for (int i = 0; i < net.layer_size(); i++) {
            auto& l = nodes[i];

            if (l.layer->bottom_size() > 0 && blob_table[l.layer->bottom(0)]) {
                auto& bottom = blob_table[l.layer->bottom(0)];
                l.prev = bottom;
                layer_table[bottom->layer->name()]->next = &l;
            }

            if (l.layer->top_size() > 0) {
                blob_table[l.layer->top(0)] = &l;
            }
        }

        auto root = std::find_if(nodes.begin(), nodes.end(), [](const layer_node& n) { return n.prev == 0; });
        if (root == nodes.end())
            throw std::runtime_error("root layer not found");
        root_node = &*root;

        const layer_node *current = &*root;
        while (current) {
            node_list.push_back(current->layer);
            current = current->next;
        }
    }

    size_t size() const {
        return node_list.size();
    }

    const caffe::LayerParameter& operator [] (size_t index) const {
        return *(node_list[index]);
    }

private:
    void upgradev1net(const caffe::NetParameter& old, caffe::NetParameter *dst) const {
        dst->CopyFrom(old);
        dst->clear_layers();
        dst->clear_layer();

        for (int i = 0; i < old.layers_size(); i++)
            upgradev1layer(old.layers(i), dst->add_layer());
    }

    const char* v1type2name(caffe::V1LayerParameter_LayerType type) const {
        switch (type) {
        case caffe::V1LayerParameter_LayerType_NONE:
            return "";
        case caffe::V1LayerParameter_LayerType_ABSVAL:
            return "AbsVal";
        case caffe::V1LayerParameter_LayerType_ACCURACY:
            return "Accuracy";
        case caffe::V1LayerParameter_LayerType_ARGMAX:
            return "ArgMax";
        case caffe::V1LayerParameter_LayerType_BNLL:
            return "BNLL";
        case caffe::V1LayerParameter_LayerType_CONCAT:
            return "Concat";
        case caffe::V1LayerParameter_LayerType_CONTRASTIVE_LOSS:
            return "ContrastiveLoss";
        case caffe::V1LayerParameter_LayerType_CONVOLUTION:
            return "Convolution";
        case caffe::V1LayerParameter_LayerType_DECONVOLUTION:
            return "Deconvolution";
        case caffe::V1LayerParameter_LayerType_DATA:
            return "Data";
        case caffe::V1LayerParameter_LayerType_DROPOUT:
            return "Dropout";
        case caffe::V1LayerParameter_LayerType_DUMMY_DATA:
            return "DummyData";
        case caffe::V1LayerParameter_LayerType_EUCLIDEAN_LOSS:
            return "EuclideanLoss";
        case caffe::V1LayerParameter_LayerType_ELTWISE:
            return "Eltwise";
        case caffe::V1LayerParameter_LayerType_EXP:
            return "Exp";
        case caffe::V1LayerParameter_LayerType_FLATTEN:
            return "Flatten";
        case caffe::V1LayerParameter_LayerType_HDF5_DATA:
            return "HDF5Data";
        case caffe::V1LayerParameter_LayerType_HDF5_OUTPUT:
            return "HDF5Output";
        case caffe::V1LayerParameter_LayerType_HINGE_LOSS:
            return "HingeLoss";
        case caffe::V1LayerParameter_LayerType_IM2COL:
            return "Im2col";
        case caffe::V1LayerParameter_LayerType_IMAGE_DATA:
            return "ImageData";
        case caffe::V1LayerParameter_LayerType_INFOGAIN_LOSS:
            return "InfogainLoss";
        case caffe::V1LayerParameter_LayerType_INNER_PRODUCT:
            return "InnerProduct";
        case caffe::V1LayerParameter_LayerType_LRN:
            return "LRN";
        case caffe::V1LayerParameter_LayerType_MEMORY_DATA:
            return "MemoryData";
        case caffe::V1LayerParameter_LayerType_MULTINOMIAL_LOGISTIC_LOSS:
            return "MultinomialLogisticLoss";
        case caffe::V1LayerParameter_LayerType_MVN:
            return "MVN";
        case caffe::V1LayerParameter_LayerType_POOLING:
            return "Pooling";
        case caffe::V1LayerParameter_LayerType_POWER:
            return "Power";
        case caffe::V1LayerParameter_LayerType_RELU:
            return "ReLU";
        case caffe::V1LayerParameter_LayerType_SIGMOID:
            return "Sigmoid";
        case caffe::V1LayerParameter_LayerType_SIGMOID_CROSS_ENTROPY_LOSS:
            return "SigmoidCrossEntropyLoss";
        case caffe::V1LayerParameter_LayerType_SILENCE:
            return "Silence";
        case caffe::V1LayerParameter_LayerType_SOFTMAX:
            return "Softmax";
        case caffe::V1LayerParameter_LayerType_SOFTMAX_LOSS:
            return "SoftmaxWithLoss";
        case caffe::V1LayerParameter_LayerType_SPLIT:
            return "Split";
        case caffe::V1LayerParameter_LayerType_SLICE:
            return "Slice";
        case caffe::V1LayerParameter_LayerType_TANH:
            return "TanH";
        case caffe::V1LayerParameter_LayerType_WINDOW_DATA:
            return "WindowData";
        case caffe::V1LayerParameter_LayerType_THRESHOLD:
            return "Threshold";
        default:
            throw std::runtime_error("unknown v1 layer-type");
        }
    }

    void upgradev1layer(const caffe::V1LayerParameter& old, caffe::LayerParameter *dst) const {
        dst->Clear();

        for (int i = 0; i < old.bottom_size(); i++)
            dst->add_bottom(old.bottom(i));

        for (int i = 0; i < old.top_size(); i++)
            dst->add_top(old.top(i));

        if (old.has_name()) dst->set_name(old.name());
        if (old.has_type()) dst->set_type(v1type2name(old.type()));

        for (int i = 0; i < old.blobs_size(); i++)
            dst->add_blobs()->CopyFrom(old.blobs(i));

        for (int i = 0; i < old.param_size(); i++) {
            while (dst->param_size() <= i) dst->add_param();
            dst->mutable_param(i)->set_name(old.param(i));
        }
        #define COPY_PARAM(name) if (old.has_##name##_param()) dst->mutable_##name##_param()->CopyFrom(old.##name##_param())

        COPY_PARAM(accuracy);
        COPY_PARAM(argmax);
        COPY_PARAM(concat);
        COPY_PARAM(contrastive_loss);
        COPY_PARAM(convolution);
        COPY_PARAM(data);
        COPY_PARAM(dropout);
        COPY_PARAM(dummy_data);
        COPY_PARAM(eltwise);
        COPY_PARAM(exp);
        COPY_PARAM(hdf5_data);
        COPY_PARAM(hdf5_output);
        COPY_PARAM(hinge_loss);
        COPY_PARAM(image_data);
        COPY_PARAM(infogain_loss);
        COPY_PARAM(inner_product);
        COPY_PARAM(lrn);
        COPY_PARAM(memory_data);
        COPY_PARAM(mvn);
        COPY_PARAM(pooling);
        COPY_PARAM(power);
        COPY_PARAM(relu);
        COPY_PARAM(sigmoid);
        COPY_PARAM(softmax);
        COPY_PARAM(slice);
        COPY_PARAM(tanh);
        COPY_PARAM(threshold);
        COPY_PARAM(window_data);
        COPY_PARAM(transform);
        COPY_PARAM(loss);
        #undef COPY_PARAM
    }

    caffe::NetParameter net;
    layer_node *root_node;
    std::map<std::string, layer_node*> layer_table; // layer name -> layer
    std::map<std::string, layer_node*> blob_table; // blob name -> bottom holder
    std::vector<layer_node> nodes;
    std::vector<const caffe::LayerParameter*> node_list;
};

} // namespace detail

/////////////////////////////////////////////////////////////////////////////////////

/**
* create whole network and load weights from caffe's netparameter
*
* @param layer [in] netparameter of caffemodel
* @param data_shape [in] size of input data (width x height x channels)
*/
inline std::shared_ptr<tiny_cnn::network<tiny_cnn::mse, tiny_cnn::adagrad>>
create_net_from_caffenet(const caffe::NetParameter& layer, const tiny_cnn::layer_shape_t& data_shape)
{
    tiny_cnn::detail::caffe_layer_vector src_net(layer);
    shape_t shape;

    if (data_shape.size() > 0) {
        shape = data_shape;
    }
    else {
        if (layer.input_shape_size() == 0)
            throw std::runtime_error("input_shape not found in caffemodel. must specify input shape explicitly");
        int depth = static_cast<int>(layer.input_shape(0).dim(1));
        int width = static_cast<int>(layer.input_shape(0).dim(2));
        int height = static_cast<int>(layer.input_shape(0).dim(3));
        shape = tiny_cnn::layer_shape_t(width, height, depth);
    }

    auto dst_net = std::make_shared<tiny_cnn::network<tiny_cnn::mse, tiny_cnn::adagrad>>(layer.name());

    for (size_t i = 0; i < src_net.size(); i++) {
        auto type = src_net[i].type();

        if (tiny_cnn::detail::layer_skipped(type)) {
            continue;
        }

        if (!tiny_cnn::detail::layer_supported(type))
            throw std::runtime_error("error: tiny-cnn does not support this layer type:" + type);

        shape_t shape_next = shape;
        auto layer = tiny_cnn::detail::create(src_net[i], shape, &shape_next);

        std::cout << "convert " << type << " => " << typeid(*layer).name() << std::endl;
        std::cout << " shape:" << shape_next << std::endl;

        dst_net->add(layer);
        shape = shape_next;
    }

    return dst_net;
}

/**
* create whole network and load weights from caffe's netparameter
*
* @param layer [in] netparameter of caffemodel
* @param data_shape [in] size of input data (width x height x channels)
*/
inline std::shared_ptr<tiny_cnn::network<tiny_cnn::mse, tiny_cnn::adagrad>>
create_net_from_caffenet(const std::string& caffebinarymodel, const tiny_cnn::layer_shape_t& data_shape)
{
    /*std::ifstream ifs(caffebinarymodel.c_str(), std::ios::in | std::ios::binary);
    caffe::NetParameter np;

    if (ifs.fail() || ifs.bad())
        throw std::runtime_error("failed to open file:" + caffebinarymodel);

    if (!np.ParseFromIstream(&ifs))
        throw std::runtime_error("failed to parse");*/

    int fd = _open(caffebinarymodel.c_str(), _O_RDONLY | _O_BINARY);
    google::protobuf::io::FileInputStream rawstr(fd);
    google::protobuf::io::CodedInputStream codedstr(&rawstr);

    codedstr.SetTotalBytesLimit(std::numeric_limits<int>::max(), std::numeric_limits<int>::max()/2);

    caffe::NetParameter np;

    if (!np.ParseFromCodedStream(&codedstr)) {
        _close(fd);
        throw std::runtime_error("failed to parse");
    }

    _close(fd);
    return create_net_from_caffenet(np, data_shape);
}

/**
* create whole network and load weights from caffe's netparameter
*
* @param layer [in] netparameter of caffe prototxt
*/
inline std::shared_ptr<tiny_cnn::network<tiny_cnn::mse, tiny_cnn::adagrad>>
create_net_from_caffeproto(const std::string& caffeprototxt)
{
int fd = _open(caffeprototxt.c_str(), _O_RDONLY);
if (fd == -1)
    throw std::runtime_error("file not fonud: " + caffeprototxt);

caffe::NetParameter np;

google::protobuf::io::FileInputStream input(fd);
input.SetCloseOnDelete(true);

if (!google::protobuf::TextFormat::Parse(&input, &np))
    throw std::runtime_error("failed to parse");

return create_net_from_caffenet(np, tiny_cnn::layer_shape_t());
}

template <typename E, typename O>
inline void load_weight_from_caffemodel(const caffe::NetParameter& layer, tiny_cnn::network<E, O> *net)
{
::detail::caffe_layer_vector src_net(layer);

int tinycnn_layer_idx = 0;

for (int caffe_layer_idx = 0; caffe_layer_idx < src_net.size(); caffe_layer_idx++) {
    auto type = src_net[caffe_layer_idx].type();

    if (::detail::layer_skipped(type) || !::detail::layer_has_weights(type)) {
        continue;
    }

    if (!::detail::layer_supported(type))
        throw std::runtime_error("error: tiny-cnn does not support this layer type:" + type);

    while (tinycnn_layer_idx < net->depth() && !::detail::layer_match(type, (*net)[tinycnn_layer_idx]->layer_type())) {
        tinycnn_layer_idx++;
    }
    if (tinycnn_layer_idx >= net->depth()) break;

    // load weight
    ::detail::load(src_net[caffe_layer_idx], (*net)[tinycnn_layer_idx]);
}
}

template <typename E, typename O>
inline void load_weight_from_caffemodel(const std::string& caffeprototxt, tiny_cnn::network<E, O> *net)
{
int fd = _open(caffeprototxt.c_str(), O_RDONLY);
if (fd == -1)
    throw std::runtime_error("file not fonud: " + caffeprototxt);

caffe::NetParameter np;

google::protobuf::io::FileInputStream input(fd);
input.SetCloseOnDelete(true);

if (!google::protobuf::TextFormat::Parse(&input, &np))
    throw std::runtime_error("failed to parse");

return load_weight_from_caffemodel(np, net);
}

} // namespace tiny_cnn