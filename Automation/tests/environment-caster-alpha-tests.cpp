#include "../../AfxHookSource2/Filmmaker/Environment/EnvironmentCasterAlphaMath.h"
#include <cstdlib>
#include <iostream>
using namespace Filmmaker::Environment;
void Check(bool value,const char* message){if(!value){std::cerr<<message<<'\n';std::exit(1);}}
int main(){
    const std::vector<unsigned char> leaves{0,8,200,255},same=leaves,changed{0,8,201,255};
    CasterAlphaImageIndex images;
    const auto hash=CasterAlphaFingerprint(2,2,leaves);
    Check(images.Intern(2,2,leaves,hash)==0,"First image creates a slot");
    Check(images.Intern(2,2,same,CasterAlphaFingerprint(2,2,same))==0,"Identical bytes in another batch share a slot");
    Check(images.Intern(2,2,changed,hash)==1,"Hash collision cannot alias different texels");
    Check(images.Intern(1,4,leaves,hash)==2,"Equal bytes with different dimensions cannot alias");
    Check(images.Intern(2,2,same,hash)==0,"Collision bucket retains the original identity");
    Check(images.Size()==3,"Only identical dimension/byte payloads were shared");
    CasterAlphaImageIndex nextMap;
    Check(nextMap.Intern(2,2,changed,CasterAlphaFingerprint(2,2,changed))==0,"A new upload does not retain old image pointers");
    std::cout<<"Caster alpha identity and collision handling passed\n";
}
