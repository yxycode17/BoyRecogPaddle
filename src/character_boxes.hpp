#pragma once
#include "ocr.hpp"
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <cmath>

struct CtcSpan { std::string text; int begin, end; };

// CTC 时间位置是对齐估计，不是字形检测。词条含多个 Unicode 字符时仅在词条内均分。
inline void CharacterBoxes(TextItem& item, const std::vector<CtcSpan>& spans, int steps,
                           int inputWidth, int resizedWidth, cv::Size crop, const cv::Mat& inverse) {
    std::vector<std::string> letters;
    std::vector<double> edges{0};
    bool aligned=steps>0 && resizedWidth>0;
    for(size_t i=0;i<spans.size();++i){
        const auto& span=spans[i];
        std::vector<std::string> token;
        // 固定并校验哈希的字典使用 UTF8；续字节不能成为新字符。
        for(size_t p=0;p<span.text.size();){
            size_t end=p+1;
            while(end<span.text.size()&&(static_cast<unsigned char>(span.text[end])&0xc0)==0x80)++end;
            token.push_back(span.text.substr(p,end-p));p=end;
        }
        double center=(span.begin+span.end)*.5;
        if(center*inputWidth>double(steps)*resizedWidth)aligned=false;
        double right=i+1==spans.size()?crop.width:
            (center+(spans[i+1].begin+spans[i+1].end)*.5)*.5*inputWidth/steps*crop.width/resizedWidth;
        double left=edges.back();
        if(right<=left||right>crop.width)aligned=false;
        for(size_t k=0;k<token.size();++k){letters.push_back(token[k]);edges.push_back(left+(right-left)*(k+1)/token.size());}
    }
    if(letters.empty())return;
    // ponytail: 无可靠 CTC 对齐时按整行均分；不增加二次识别或额外模型。
    if(!aligned)for(size_t i=0;i<edges.size();++i)edges[i]=double(crop.width)*i/letters.size();
    item.characters.reserve(letters.size());
    for(size_t i=0;i<letters.size();++i){
        std::vector<cv::Point2f> points{{float(edges[i]),0},{float(edges[i+1]),0},
            {float(edges[i+1]),float(crop.height)},{float(edges[i]),float(crop.height)}};
        cv::perspectiveTransform(points,points,inverse);
        float l=points[0].x,r=l,t=points[0].y,b=t;
        for(auto p:points){l=std::min(l,p.x);r=std::max(r,p.x);t=std::min(t,p.y);b=std::max(b,p.y);}
        int x=std::clamp(int(std::floor(l)),item.rect.x,item.rect.x+item.rect.width-1);
        int y=std::clamp(int(std::floor(t)),item.rect.y,item.rect.y+item.rect.height-1);
        int right=std::clamp(int(std::ceil(r)),x+1,item.rect.x+item.rect.width);
        int bottom=std::clamp(int(std::ceil(b)),y+1,item.rect.y+item.rect.height);
        item.characters.push_back({std::move(letters[i]),{x,y,right-x,bottom-y}});
    }
}
