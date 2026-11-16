#include "../src/character_boxes.hpp"
#include <opencv2/geometry.hpp>
#include <cassert>
#include <iostream>

int main(){
    cv::Mat identity=cv::Mat::eye(3,3,CV_64F);
    auto make=[&](std::vector<CtcSpan> spans,int steps,int width,int resized,cv::Size crop,cv::Mat inverse,cv::Rect bounds){
        TextItem item;item.rect=bounds;
        CharacterBoxes(item,spans,steps,width,resized,crop,inverse);
        for(const auto& c:item.characters)assert(c.rect.area()>0&&(c.rect&bounds)==c.rect);
        return item.characters;
    };
    // 非均匀 CTC 中心；批量右侧补边不得扩大字符位置。
    auto chars=make({{"W",1,3},{"i",5,6},{"中",7,9}},20,200,100,{100,20},identity,{0,0,100,20});
    assert(chars.size()==3&&chars[0].rect==cv::Rect(0,0,38,20));
    assert(chars[1].rect==cv::Rect(37,0,31,20)&&chars[2].rect==cv::Rect(67,0,33,20));
    // 多码点词条均分，非 BMP 字符保持完整；无效补边对齐整行均分。
    chars=make({{"🇨🇳",1,2}},10,100,100,{100,20},identity,{0,0,100,20});
    assert(chars.size()==2&&chars[0].text=="🇨"&&chars[1].text=="🇳"&&chars[0].rect.width==50);
    chars=make({{"a",1,2},{"b",18,20}},20,200,100,{100,20},identity,{0,0,100,20});
    assert(chars[0].rect.width==50&&chars[1].rect.x==50);
    // 旋转/透视逆映射：半开边界，包含纵向 crop 自动旋转的逆变换。
    std::vector<cv::Point2f> source{{0,0},{100,0},{100,20},{0,20}};
    std::vector<std::vector<cv::Point2f>> targets{
        {{10,10},{110,10},{110,30},{10,30}},
        {{30,10},{30,110},{10,110},{10,10}},
        {{110,30},{10,30},{10,10},{110,10}},
        {{10,110},{10,10},{30,10},{30,110}},
        {{10,10},{110,20},{105,40},{10,30}}};
    for(const auto& target:targets){
        auto matrix=cv::getPerspectiveTransform(source,target);
        chars=make({{"a",1,3},{"b",7,9}},10,100,100,{100,20},matrix,{0,0,120,120});
        assert(chars.size()==2);
        std::vector<cv::Point2f> center{{25,10},{75,10}};cv::perspectiveTransform(center,center,matrix);
        for(int i=0;i<2;++i)assert(chars[i].rect.contains(cv::Point(int(center[i].x),int(center[i].y))));
    }
    std::cout<<"Character alignment, padding, Unicode fallback and inverse geometry passed\n";
}
