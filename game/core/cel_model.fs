#version 330

in vec3 fragPosition;
in vec2 fragTexCoord;
in vec4 fragColor;
in vec3 fragNormal;

uniform sampler2D texture0;
uniform vec4 colDiffuse;

uniform vec3 u_light_dir;
uniform vec3 u_light_color;
uniform vec3 u_ambient_color;
uniform vec3 u_view_pos;
uniform float u_rim_power;
uniform vec3 u_rim_color;

out vec4 finalColor;

void main()
{
    vec4 texelColor = texture(texture0, fragTexCoord);
    vec4 baseColor = texelColor * colDiffuse * fragColor;

    if (baseColor.a < 0.1) discard;

    vec3 N = normalize(fragNormal);
    if (u_rim_power <= 0.01)
    {
        vec3 dX = dFdx(fragPosition);
        vec3 dY = dFdy(fragPosition);
        vec3 crossVal = cross(dX, dY);
        float len = length(crossVal);
        if (len > 1e-7)
        {
            N = crossVal / len;
            if (dot(N, fragNormal) < 0.0) N = -N;
        }
    }

    vec3 L = normalize(u_light_dir);
    vec3 toView = u_view_pos - fragPosition;
    float distV = length(toView);
    vec3 V = (distV > 1e-4) ? (toView / distV) : vec3(0.0, 1.0, 0.0);
    vec3 toH = L + V;
    float distH = length(toH);
    vec3 H = (distH > 1e-4) ? (toH / distH) : L;

    float NdotL = dot(N, L);
    float lightCutoff = 0.0;
    float falloff = 0.05;
    float lightMask = smoothstep(lightCutoff, lightCutoff + falloff, NdotL);

    vec3 shadowColor = baseColor.rgb * u_ambient_color;
    vec3 litColor = baseColor.rgb * u_light_color;
    vec3 diffuse = mix(shadowColor, litColor, lightMask);

    vec3 specular = vec3(0.0);
    if (u_rim_power > 0.01)
    {
        float NdotH = clamp(dot(N, H), 0.0, 1.0);
        float spec = pow(NdotH, 64.0);
        float specMask = smoothstep(0.20, 0.20 + falloff, spec) * lightMask;
        specular = u_light_color * specMask * 0.65;
    }

    vec3 rimLight = vec3(0.0);
    if (u_rim_power > 0.01)
    {
        float NdotV = clamp(dot(N, V), 0.0, 1.0);
        float rimExp = 1.0 / max(u_rim_power, 0.05);
        float fresnel = pow(1.0 - NdotV, rimExp);
        float fresnelLit = fresnel * (max(NdotL, 0.0) * 0.5 + 0.5);
        float rimMask = smoothstep(0.15, 0.15 + falloff, fresnelLit);
        rimLight = u_rim_color * rimMask * 0.45;
    }

    vec3 result = clamp(diffuse + specular + rimLight, 0.0, 2.0);
    finalColor = vec4(result, baseColor.a);
}
